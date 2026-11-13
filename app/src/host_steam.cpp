// The settings app's Steam Workshop, in the Steam edition only: what the discover page asks of
// Steam (D68), and the Workshop's themes in the library, installed with one administrator's
// approval for as many as were chosen (D62). Everything Steam is asked goes through
// animelogon-steam.exe (steam_client.h, steam/PROTOCOL.md); its answers come back on this window's
// message loop, matched to what was asked by their ids.
#include "host.h"

#include <shellapi.h>

#include <algorithm>

#include "animelogon/components.h"
#include "animelogon/i18n.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "commit.h"
#include "steam_library.h"

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace app {

namespace {

// The folder of the stand-in for Steam's library's fixture, for its preview pictures while
// developing (steam/PROTOCOL.md). The page's CSP allows pictures from it; it is mapped only when
// ANIMELOGON_FAKE_STEAM names a fixture.
constexpr wchar_t kFixtureHost[] = L"steam-fixture.animelogon.invalid";

// The sorts a query may ask for (steam/PROTOCOL.md).
bool IsSort(const std::wstring &s) { return s == L"trend" || s == L"recent" || s == L"subscribed" || s == L"rating"; }

// A value read from the helper, written again as it was.
void Put(json::Writer &w, const json::Value &v) {
    switch (v.type()) {
    case json::Value::Type::Null: w.Null(); break;
    case json::Value::Type::Bool: w.Bool(v.Bool()); break;
    case json::Value::Type::Number: w.Number(v.Num()); break;
    case json::Value::Type::String: w.String(v.Str()); break;
    case json::Value::Type::Array:
        w.BeginArray();
        for (const json::Value &item : v.Items()) Put(w, item);
        w.EndArray();
        break;
    case json::Value::Type::Object:
        w.BeginObject();
        for (const auto &[key, member] : v.Members()) {
            w.Key(key);
            Put(w, member);
        }
        w.EndObject();
        break;
    }
}

// The answer to a request the helper could not be given, or that it went without answering.
json::Value Failure(const wchar_t *error) {
    json::Value v;
    json::Parse(std::wstring(L"{\"ok\":false,\"error\":\"") + error + L"\"}", &v);
    return v;
}

// Tags as the page sends them: a few short names, which Steam matches as they are.
void Tags(json::Writer &w, const wchar_t *key, const json::Value &list) {
    if (!list.IsArray()) return;
    w.Key(key).BeginArray();
    size_t n = 0;
    for (const json::Value &t : list.Items())
        if (t.IsString() && !t.Str().empty() && t.Str().size() <= 64 && ++n <= 16) w.String(t.Str());
    w.EndArray();
}

std::wstring FileStem(const std::wstring &path) {
    const size_t slash = path.find_last_of(L'\\');
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

// When an installed theme's folder was made, to tell apart themes installed on one command line.
unsigned long long MadeAt(const std::wstring &theme) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ThemeDir(theme).c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((unsigned long long)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
}

}  // namespace

void Host::StartSteam() {
    const std::wstring helper = steam::HelperIn(paths::ModuleDir());
    if (helper.empty()) return;
    const HWND hwnd = hwnd_;
    steam_ = std::make_unique<steam::Client>(helper, steam::AppId(), [hwnd] { PostMessageW(hwnd, kSteamLines, 0, 0); });
    ALOG(L"steam: the Steam edition, app %u", steam_->appid());
}

void Host::StopSteam() {
    if (!steam_) return;
    steam_->Stop();
    steamWaiting_.clear();
}

void Host::MapSteamFixture(ICoreWebView2 *view) {
    if (!steam_ || !view) return;
    wchar_t fixture[MAX_PATH * 2] = L"";
    const DWORD n = GetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM", fixture, ARRAYSIZE(fixture));
    if (!n || n >= ARRAYSIZE(fixture)) return;
    std::wstring dir = fixture;
    dir.resize(dir.find_last_of(L"\\/") == std::wstring::npos ? 0 : dir.find_last_of(L"\\/"));
    ComPtr<ICoreWebView2_3> view3;
    if (dir.empty() || FAILED(view->QueryInterface(IID_PPV_ARGS(&view3)))) return;
    if (SUCCEEDED(view3->SetVirtualHostNameToFolderMapping(kFixtureHost, dir.c_str(),
                                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
        ALOG(L"steam: the stand-in's fixture folder %s is https://%s/", dir.c_str(), kFixtureHost);
}

long long Host::SteamAsk(const SteamRequest &request, const std::function<void(json::Writer &)> &params) {
    const long long id = steam_ ? steam_->Request(request.kind, params) : 0;
    if (id) steamWaiting_[id] = request;
    else SteamAnswered(request, Failure(L"helper_failed"));
    return id;
}

bool Host::OnSteamMessage(const std::wstring &op, const json::Value &m) {
    // The GitHub edition has no Workshop: whatever the page asks about it, nothing happens.
    if (!steam_) return op.rfind(L"steam", 0) == 0;
    const std::wstring item = m[L"itemId"].Str();
    const bool named = steam::IsItemId(item);
    if (op == L"steamStatus") {
        SteamAsk({L"status"});
    } else if (op == L"steamQuery") {
        SteamRequest r{L"query", (int)m[L"seq"].Num()};
        SteamAsk(r, [&m](json::Writer &w) {
            const std::wstring sort = m[L"sort"].Str();
            w.Member(L"sort", IsSort(sort) ? sort.c_str() : L"trend");
            w.Member(L"trendDays", (int)std::clamp(m[L"trendDays"].Num(7), 1.0, 365.0));
            const std::wstring text = m[L"text"].Str();
            if (!text.empty()) w.Member(L"text", text.substr(0, 256));
            Tags(w, L"requiredTags", m[L"requiredTags"]);
            Tags(w, L"excludedTags", m[L"excludedTags"]);
            w.Member(L"matchAnyTag", m[L"matchAnyTag"].Bool());
            w.Member(L"includeMature", m[L"includeMature"].Bool());
            w.Member(L"page", (int)std::clamp(m[L"page"].Num(1), 1.0, 100000.0));
            w.Member(L"perPage", (int)std::clamp(m[L"perPage"].Num(50), 1.0, 50.0));
        });
    } else if (op == L"steamDetails") {
        if (named) SteamAsk({L"details", 0, item}, [&item](json::Writer &w) { w.Member(L"itemId", item); });
    } else if (op == L"steamSubscribe") {
        const bool on = m[L"on"].Bool();
        if (named) SteamAsk({on ? L"subscribe" : L"unsubscribe", 0, item, on}, [&item](json::Writer &w) { w.Member(L"itemId", item); });
    } else if (op == L"steamDownload") {
        if (named) {
            steamProblems_.erase(item);
            SteamDownload(item, true);
        }
    } else if (op == L"steamItems") {
        AskSubscribed();
    } else if (op == L"steamInstall") {
        InstallFromSteam(m[L"items"]);
    } else if (op == L"steamOpen") {
        // Steam's own pages, opened in Steam: the Workshop, an item's page, or Steam itself.
        const std::wstring what = m[L"what"].Str();
        std::wstring url;
        if (what == L"workshop") url = L"steam://url/SteamWorkshopPage/" + std::to_wstring(steam_->appid());
        else if (what == L"item" && named) url = L"steam://url/CommunityFilePage/" + item;
        else if (what == L"steam") url = L"steam://open/main";
        if (url.empty()) return true;
        SHELLEXECUTEINFOW ei{sizeof(ei)};
        ei.fMask = SEE_MASK_FLAG_NO_UI;
        ei.hwnd = hwnd_;
        ei.lpVerb = L"open";
        ei.lpFile = url.c_str();
        ei.nShow = SW_SHOWNORMAL;
        ALOG(L"steam: opening %s", url.c_str());
        if (!ShellExecuteExW(&ei)) Problem(i18n::Tr("msg.steam.open_failed"));
    } else {
        return false;
    }
    return true;
}

void Host::SteamDownload(const std::wstring &item, bool hurry) {
    steamDownloadAsked_.insert(item);
    SteamAsk({L"download", 0, item}, [&item, hurry](json::Writer &w) { w.Member(L"itemId", item).Member(L"highPriority", hurry); });
}

void Host::OnSteamLines() {
    if (!steam_) return;
    bool exited = false;
    for (const json::Value &v : steam_->Take(&exited)) {
        if (v[L"event"].IsString()) {
            SteamEvent(v);
            continue;
        }
        if (!v[L"id"].IsNumber()) continue;
        const auto it = steamWaiting_.find((long long)v[L"id"].Num());
        if (it == steamWaiting_.end()) continue;
        const SteamRequest r = it->second;
        steamWaiting_.erase(it);
        SteamAnswered(r, v);
    }
    if (!exited) return;
    // Gone without answering what was still asked: each of those fails, and the page hears it.
    std::map<long long, SteamRequest> waiting;
    waiting.swap(steamWaiting_);
    steamDownloads_.clear();
    steamState_ = L"helper_exited";
    for (const auto &[id, r] : waiting)
        if (r.kind != L"status") SteamAnswered(r, Failure(L"helper_exited"));
    SendSteamStatus(Failure(L"helper_exited"));
}

void Host::SendSteamStatus(const json::Value &v) {
    json::Writer w;
    const bool ok = v[L"ok"].Bool(true);
    steamState_ = ok ? v[L"steam"].Str(L"not_running") : v[L"error"].Str(L"helper_failed");
    w.BeginObject().Member(L"type", L"steamStatus").Member(L"steam", steamState_);
    w.Member(L"message", v[L"message"].Str());
    w.Member(L"loggedOn", v[L"loggedOn"].Bool(true)).Member(L"ownsApp", v[L"ownsApp"].Bool(true));
    w.Member(L"appid", (double)steam_->appid());
    w.EndObject();
    Send(w);
}

void Host::SteamAnswered(const SteamRequest &r, const json::Value &v) {
    const bool ok = v[L"ok"].Bool();
    auto failed = [&v](json::Writer &w) {
        w.Member(L"ok", v[L"ok"].Bool()).Member(L"error", v[L"error"].Str()).Member(L"message", v[L"message"].Str());
    };
    if (r.kind == L"status") {
        SendSteamStatus(v);
    } else if (r.kind == L"query") {
        json::Writer w;
        w.BeginObject().Member(L"type", L"steamQuery").Member(L"seq", r.seq);
        failed(w);
        if (ok) {
            w.Member(L"total", v[L"total"].Num()).Member(L"hidden", v[L"hidden"].Num());
            w.Key(L"next");
            Put(w, v[L"next"]);
            w.Key(L"items");
            Put(w, v[L"items"].IsArray() ? v[L"items"] : json::Value());
        }
        w.EndObject();
        Send(w);
    } else if (r.kind == L"details") {
        json::Writer w;
        w.BeginObject().Member(L"type", L"steamDetails").Member(L"itemId", r.item);
        failed(w);
        if (ok) {
            w.Key(L"item");
            Put(w, v[L"item"]);
        }
        w.EndObject();
        Send(w);
    } else if (r.kind == L"subscribe" || r.kind == L"unsubscribe") {
        json::Writer w;
        w.BeginObject().Member(L"type", L"steamSubscribe").Member(L"itemId", r.item).Member(L"on", r.on);
        failed(w);
        w.EndObject();
        Send(w);
        // Subscribed: its files are fetched at once, so that it can be installed (D62).
        if (ok && r.on) {
            steamProblems_.erase(r.item);
            SteamDownload(r.item, true);
        }
        if (ok) AskSubscribed();
    } else if (r.kind == L"download") {
        if (ok) {
            steamDownloads_.emplace(r.item, std::make_pair(0.0, 0.0));
        } else {
            steamDownloads_.erase(r.item);
            steamProblems_[r.item] = i18n::Tr("steam.problem.download", {{L"why", v[L"message"].Str(v[L"error"].Str())}});
        }
        if (!steamShown_.empty()) SendSteamItems();
    } else if (r.kind == L"subscribed") {
        steamItemsAsked_ = false;
        if (ok) {
            steamFromHelper_ = true;
            steamItems_ = steam_workshop::FromSubscribed(v);
            // What Steam has not downloaded yet, or has a newer version of, is fetched now, as
            // Steam itself would when the app runs; each once while this window is open.
            for (const steam_workshop::Item &i : steamItems_)
                if (i.subscribed && (!i.installed || i.needsUpdate) && !i.downloading && !steamDownloadAsked_.count(i.id))
                    SteamDownload(i.id, false);
        } else {
            // Without Steam: what it downloaded to this computer's libraries.
            steamFromHelper_ = false;
            steamItems_ = steam_workshop::FromFolders(steam_library::WorkshopContent(steam_->appid()));
            if (!v[L"error"].Str().empty()) steamState_ = v[L"error"].Str();
        }
        SendSteamItems();
        if (steamItemsAgain_) {
            steamItemsAgain_ = false;
            AskSubscribed();
        }
    }
}

void Host::SteamEvent(const json::Value &v) {
    const std::wstring event = v[L"event"].Str(), id = v[L"id"].Str();
    if (event == L"progress" && v[L"op"].Str() == L"download") {
        if (!steam::IsItemId(id)) return;
        steamDownloads_[id] = {v[L"done"].Num(), v[L"total"].Num()};
        json::Writer w;
        w.BeginObject().Member(L"type", L"steamProgress").Member(L"id", id);
        w.Member(L"done", v[L"done"].Num()).Member(L"total", v[L"total"].Num()).EndObject();
        Send(w);
    } else if (event == L"installed") {
        steamDownloads_.erase(id);
        steamProblems_.erase(id);
        json::Writer w;
        w.BeginObject().Member(L"type", L"steamInstalled").Member(L"id", id).EndObject();
        Send(w);
        AskSubscribed();
    } else if (event == L"downloadFailed") {
        steamDownloads_.erase(id);
        steamProblems_[id] = i18n::Tr("steam.problem.download", {{L"why", v[L"message"].Str()}});
        if (!steamShown_.empty()) SendSteamItems();
    } else if (event == L"subscriptionsChanged") {
        AskSubscribed();
    } else if (event == L"status") {
        SendSteamStatus(v);
        steamDownloads_.clear();
        AskSubscribed();
    }
}

void Host::AskSubscribed() {
    if (!steam_) return;
    if (steamItemsAsked_) {
        steamItemsAgain_ = true;
        return;
    }
    steamItemsAsked_ = true;
    SteamAsk({L"subscribed"});
}

void Host::SendSteamItems() {
    if (!steam_) return;
    const steam_workshop::Records records = steam_workshop::LoadRecords(steam_workshop::RecordsPath());
    std::vector<SteamShown> shown;
    std::set<std::wstring> listed;
    for (const steam_workshop::Item &item : steamItems_) {
        if (!listed.insert(item.id).second) continue;
        SteamShown s;
        s.item = item;
        s.folder = steam_workshop::Inspect(item.folder);
        const auto rec = records.find(item.id);
        if (rec != records.end()) {
            s.record = rec->second;
            s.recorded = true;
        }
        if (steamDownloads_.count(item.id)) s.item.downloading = true;
        const bool themeThere = s.recorded && library_.Entry(s.record.theme);
        s.state = steam_workshop::StateOf(s.item, s.folder, s.recorded ? &s.record : nullptr, themeThere);
        // Neither subscribed nor installed: nothing to show.
        if (!s.item.subscribed && s.state != steam_workshop::State::Unsubscribed) continue;
        shown.push_back(std::move(s));
    }
    // Installed from an item no longer subscribed to: Steam does not list it, or without Steam, its
    // folder is gone. Without Steam that may also be a library that is not there now, so it is
    // shown as installed, which is all that is known.
    for (const auto &[id, record] : records) {
        if (listed.count(id) || !library_.Entry(record.theme)) continue;
        SteamShown s;
        s.item.id = id;
        s.item.title = record.title;
        s.record = record;
        s.recorded = true;
        s.item.subscribed = !steamFromHelper_;
        s.state = steamFromHelper_ ? steam_workshop::State::Unsubscribed : steam_workshop::State::Installed;
        shown.push_back(std::move(s));
    }
    steamShown_ = shown;

    json::Writer w;
    w.BeginObject().Member(L"type", L"steamItems").Member(L"source", steamFromHelper_ ? L"steam" : L"folders");
    w.Member(L"steam", steamState_).Key(L"items").BeginArray();
    for (const SteamShown &s : steamShown_) {
        const steam_workshop::State state = s.state;
        std::wstring title = s.item.title;
        if (title.empty()) title = s.record.title;
        if (title.empty() && !s.folder.package.empty()) title = FileStem(s.folder.package);
        std::wstring problem;
        if (state == steam_workshop::State::Unreadable) problem = i18n::Tr(steam_workshop::ProblemKey(s.folder.contents));
        else if (const auto p = steamProblems_.find(s.item.id); p != steamProblems_.end()) problem = p->second;
        const ThemeEntry *theme = s.recorded ? library_.Entry(s.record.theme) : nullptr;
        w.BeginObject().Member(L"id", s.item.id).Member(L"title", title).Member(L"preview", s.item.preview);
        w.Member(L"state", steam_workshop::NameOf(state)).Member(L"problem", problem);
        w.Member(L"theme", theme ? theme->id : std::wstring());
        w.Member(L"subscribed", s.item.subscribed);
        w.Member(L"size", (double)(s.folder.size ? s.folder.size : s.item.sizeOnDisk));
        w.Member(L"timeUpdated", (double)s.item.timeUpdated);
        if (const auto d = steamDownloads_.find(s.item.id); d != steamDownloads_.end())
            w.Key(L"progress").BeginObject().Member(L"done", d->second.first).Member(L"total", d->second.second).EndObject();
        w.Key(L"descriptors").BeginArray();
        for (const std::wstring &d : s.item.descriptors) w.String(d);
        w.EndArray().EndObject();
    }
    w.EndArray().EndObject();
    Send(w);
}

void Host::InstallFromSteam(const json::Value &ids) {
    if (importing_) return;
    std::vector<ImportJob> jobs;
    std::set<std::wstring> seen;
    // Only what the page was last shown, and only what can be installed now: the page names items,
    // never files.
    for (const json::Value &v : ids.Items()) {
        const std::wstring id = v.Str();
        if (!seen.insert(id).second) continue;
        const auto it = std::find_if(steamShown_.begin(), steamShown_.end(), [&id](const SteamShown &s) { return s.item.id == id; });
        if (it == steamShown_.end() || !steam_workshop::Installable(it->state)) continue;
        ImportJob job;
        job.source = it->folder.package;
        job.name = it->item.title.empty() ? it->record.title : it->item.title;
        job.item = id;
        job.updated = it->item.timeUpdated;
        job.fileTime = it->folder.time;
        if (it->state == steam_workshop::State::Update) job.replaces = it->record.theme;
        steamProblems_.erase(id);
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        Problem(i18n::Tr("msg.steam.nothing"));
        if (!steamShown_.empty()) SendSteamItems();
        return;
    }
    ALOG(L"steam: installing %zu Workshop item(s)", jobs.size());
    StartImport(std::move(jobs), false);
}

steam_workshop::Reuse Host::StagedReuse(const std::vector<size_t> &staged) const {
    std::vector<std::wstring> dirs;
    bool replacing = false;
    for (size_t k : staged) {
        dirs.push_back(importJobs_[k].dir);
        replacing = replacing || !importJobs_[k].replaces.empty();
    }
    return replacing ? steam_workshop::StagedReuse(dirs) : steam_workshop::Reuse();
}

std::wstring Host::ReplaceCommands(const ImportJob &job, const steam_workshop::Reuse &reuse) const {
    if (job.replaces.empty() || !library_.Entry(job.replaces) || library_.Entry(job.replaces)->builtIn) return L"";
    std::wstring out = L" --remove-theme " + job.replaces;
    std::vector<std::wstring> wallpapers, components;
    library_.OnlyUsedBy(job.replaces, &wallpapers, &components);
    for (const std::wstring &id : wallpapers) {
        const auto w = std::find_if(library_.wallpapers().begin(), library_.wallpapers().end(),
                                    [&id](const WallpaperInfo &i) { return i.id == id; });
        if (w != library_.wallpapers().end() && !w->builtIn && reuse.MayRemoveWallpaper(w->sha256))
            out += L" --remove-wallpaper " + id;
    }
    for (const std::wstring &id : components)
        if (reuse.MayRemoveComponent(steam_workshop::NormalizedComponent(ComponentFilePath(id))))
            out += L" --remove-component " + id;
    return out;
}

int Host::RecordSteamInstalls(const std::vector<size_t> &staged, const std::vector<std::wstring> &added,
                              const std::vector<staging::Result> &results) {
    // Which new theme came from which job: the elevated helper installs them in the order of the
    // command line, so in the order their folders were made, each the first one not yet taken that
    // has the name its staging gave it.
    std::vector<std::wstring> made = added;
    std::stable_sort(made.begin(), made.end(), [](const std::wstring &a, const std::wstring &b) { return MadeAt(a) < MadeAt(b); });
    std::vector<bool> taken(made.size());
    steam_workshop::Records records = steam_workshop::LoadRecords(steam_workshop::RecordsPath());
    int updates = 0;
    bool moved = false;
    for (size_t k : staged) {
        const ImportJob &job = importJobs_[k];
        std::wstring theme;
        for (size_t i = 0; i < made.size() && theme.empty(); ++i) {
            const ThemeEntry *e = library_.Entry(made[i]);
            if (!taken[i] && e && e->name == results[k].name) {
                taken[i] = true;
                theme = made[i];
            }
        }
        if (job.item.empty() || theme.empty()) continue;
        records[job.item] = {theme, job.name, job.updated, job.fileTime};
        if (job.replaces.empty()) continue;
        ++updates;
        // Replaced on the same command line: the update takes its place on the logon screen, with
        // the person's adjustments to it.
        if (!ThemeExists(job.replaces)) {
            library_.Move(job.replaces, theme);
            moved = true;
        }
    }
    // What was installed and has been deleted since is not kept.
    for (auto it = records.begin(); it != records.end();) it = library_.Entry(it->second.theme) ? std::next(it) : records.erase(it);
    if (!steam_workshop::SaveRecords(steam_workshop::RecordsPath(), records))
        ALOG(L"steam: %s could not be written", steam_workshop::RecordsPath().c_str());
    if (moved) Save();
    return updates;
}

}  // namespace app
