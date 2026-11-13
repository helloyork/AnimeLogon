// A stand-in for Steam's library, steam_api64.dll, for running animelogon-steam.exe without Steam:
// in the tests, and when developing the settings app's Workshop pages. It exports every entry
// point the helper looks up (steam/src/steam_flat.h), under the same names, and answers from a
// JSON fixture named by the environment variable ANIMELOGON_FAKE_STEAM; steam/PROTOCOL.md
// describes the fixture. Subscribing, downloading and publishing change the items it holds in
// memory, and their results come back through manual dispatch as Steam's do.
//
// It shares steam_flat.h with the helper, so it cannot catch a layout that differs from Valve's:
// that is checked against the SDK. What it checks is that the helper uses the API as declared.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "animelogon/text.h"
#include "json.h"
#include "steam_flat.h"

#define FAKE_EXPORT extern "C" __declspec(dllexport)

using namespace animelogon;

namespace {

struct Item {
    struct Preview {
        std::string url, name;
        int32_t type = 0;
    };
    steam::PublishedFileId id = 0;
    std::string title, description, preview;
    uint64_t owner = 76561198000000001ull;
    std::vector<std::string> tags;
    std::vector<int32_t> descriptors;
    uint32_t votesUp = 0, votesDown = 0, timeCreated = 0, timeUpdated = 0;
    float score = 0;
    uint64_t subscriptions = 0, favorites = 0, fileSize = 0, trend = 0, sizeOnDisk = 0;
    int32_t visibility = 0;
    bool subscribed = false, installed = false, needsUpdate = false;
    std::wstring folder;
    uint32_t installedAt = 0;
    unsigned long long downloadStarted = 0;  // GetTickCount64 while downloading, else 0
    std::vector<Preview> previews;
};

struct Query {
    bool details = false;
    int32_t ranking = 0;
    uint32_t page = 1;
    bool byCursor = false;
    std::string cursor, text;
    std::vector<std::string> required, excluded;
    bool matchAny = false, longDescription = false, additionalPreviews = false;
    std::vector<steam::PublishedFileId> ids;
    std::vector<int> rows;  // after sending: the index in the items of each result, or -1 when none
    std::vector<steam::PublishedFileId> rowIds;
};

struct Update {
    steam::PublishedFileId item = 0;
    bool hasTitle = false, hasDescription = false, hasVisibility = false, hasTags = false;
    std::string title, description, content, preview;
    int32_t visibility = 0;
    std::vector<std::string> tags;
    std::vector<int32_t> add, remove;
    unsigned long long submitted = 0;  // GetTickCount64 when submitted, else 0
    steam::APICall call = 0;
};

struct Message {
    steam::APICall call;  // 0 for a callback that is posted rather than a call's result
    int32_t callback;
    std::vector<uint8_t> bytes;
};

struct Fake {
    bool loaded = false, initialized = false;
    bool running = false, loggedOn = true, owns = true, needsLegalAgreement = false;
    int32_t initResult = steam::kInitOk;
    unsigned uploadMs = 600, downloadMs = 400;
    std::map<std::string, int32_t> fail;
    steam::PublishedFileId nextItem = 4000000000ull;
    std::vector<Item> items;
    std::wstring dir, log;
    steam::AppId appid = 0;
    uint32_t clock = 1700000000;

    std::vector<Message> posted;  // taken into `ready` at the next RunFrame
    std::deque<Message> ready;
    std::map<steam::APICall, Message> results;
    Message current{};
    steam::APICall nextCall = 1;
    steam::QueryHandle nextQuery = 1;
    steam::UpdateHandle nextUpdate = 1;
    std::map<steam::QueryHandle, Query> queries;
    std::map<steam::UpdateHandle, Update> updates;
};

Fake &F() {
    static Fake fake;
    return fake;
}

std::wstring Env(const wchar_t *name) {
    wchar_t value[32768];
    const DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return n && n < ARRAYSIZE(value) ? std::wstring(value, n) : L"";
}

void Log(const std::string &line) {
    if (F().log.empty()) return;
    HANDLE h = CreateFileW(F().log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string text = line + "\n";
    DWORD wrote = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
}

uint64_t ParseId(const std::wstring &text) {
    uint64_t n = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return 0;
        n = n * 10 + (uint64_t)(c - L'0');
    }
    return n;
}

const wchar_t *const kDescriptorNames[] = {L"", L"nuditySexual", L"violenceGore", L"adultOnlySexual", L"gratuitousSexual",
                                           L"generalMature"};

// The fixture: read once, at the first call that needs it.
void Load() {
    Fake &f = F();
    if (f.loaded) return;
    f.loaded = true;
    f.log = Env(L"ANIMELOGON_FAKE_STEAM_LOG");
    f.appid = (steam::AppId)ParseId(Env(L"SteamAppId"));
    const std::wstring path = Env(L"ANIMELOGON_FAKE_STEAM");
    if (path.empty()) return;
    f.dir = path.substr(0, path.find_last_of(L"\\/"));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string bytes;
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got) bytes.append(buffer, got);
    CloseHandle(h);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    json::Value v;
    if (!json::Parse(FromUtf8(bytes), &v) || !v.IsObject()) return;
    f.running = v[L"running"].Bool(true);
    f.initResult = (int32_t)v[L"initResult"].Num(steam::kInitOk);
    f.loggedOn = v[L"loggedOn"].Bool(true);
    f.owns = v[L"owns"].Bool(true);
    f.needsLegalAgreement = v[L"needsLegalAgreement"].Bool(false);
    f.uploadMs = (unsigned)v[L"uploadMs"].Num(600);
    f.downloadMs = (unsigned)v[L"downloadMs"].Num(400);
    if (v[L"nextItemId"].IsString()) f.nextItem = ParseId(v[L"nextItemId"].Str());
    for (const auto &[op, result] : v[L"fail"].Members()) f.fail[ToUtf8(op)] = (int32_t)result.Num(steam::kResultFail);
    for (const json::Value &j : v[L"items"].Items()) {
        Item item;
        item.id = ParseId(j[L"id"].Str());
        item.title = ToUtf8(j[L"title"].Str());
        item.description = ToUtf8(j[L"description"].Str());
        item.preview = ToUtf8(j[L"preview"].Str());
        if (j[L"owner"].IsString()) item.owner = ParseId(j[L"owner"].Str());
        for (const json::Value &t : j[L"tags"].Items()) item.tags.push_back(ToUtf8(t.Str()));
        for (const json::Value &d : j[L"descriptors"].Items()) {
            if (d.IsNumber()) item.descriptors.push_back((int32_t)d.Num());
            for (int32_t n = 1; n < (int32_t)ARRAYSIZE(kDescriptorNames); ++n)
                if (d.IsString() && d.Str() == kDescriptorNames[n]) item.descriptors.push_back(n);
        }
        item.votesUp = (uint32_t)j[L"votesUp"].Num();
        item.votesDown = (uint32_t)j[L"votesDown"].Num();
        item.score = (float)j[L"score"].Num();
        item.subscriptions = (uint64_t)j[L"subscriptions"].Num();
        item.favorites = (uint64_t)j[L"favorites"].Num();
        item.fileSize = (uint64_t)j[L"fileSize"].Num();
        item.trend = (uint64_t)j[L"trend"].Num((double)item.subscriptions);
        item.timeCreated = (uint32_t)j[L"timeCreated"].Num(f.clock);
        item.timeUpdated = (uint32_t)j[L"timeUpdated"].Num(item.timeCreated);
        item.visibility = (int32_t)j[L"visibility"].Num();
        item.subscribed = j[L"subscribed"].Bool();
        item.installed = j[L"installed"].Bool();
        item.needsUpdate = j[L"needsUpdate"].Bool();
        item.sizeOnDisk = (uint64_t)j[L"sizeOnDisk"].Num((double)item.fileSize);
        item.folder = j[L"folder"].Str();
        item.installedAt = item.timeUpdated;
        for (const json::Value &p : j[L"previews"].Items())
            item.previews.push_back({ToUtf8(p[L"url"].Str()), ToUtf8(p[L"name"].Str()), (int32_t)p[L"type"].Num()});
        f.items.push_back(std::move(item));
    }
}

Item *Find(steam::PublishedFileId id) {
    for (Item &i : F().items)
        if (i.id == id) return &i;
    return nullptr;
}

// The folder an installed item is in: the fixture's, beside the fixture when relative, else content\<id> there.
std::wstring FolderOf(const Item &item) {
    std::wstring folder = item.folder.empty() ? L"content\\" + std::to_wstring(item.id) : item.folder;
    const bool full = folder.size() > 2 && (folder[1] == L':' || (folder[0] == L'\\' && folder[1] == L'\\'));
    return full ? folder : F().dir + L"\\" + folder;
}

int32_t Failure(const char *op) {
    const auto it = F().fail.find(op);
    return it == F().fail.end() ? 0 : it->second;
}

template <class T>
std::vector<uint8_t> Bytes(const T &t) {
    const auto *p = reinterpret_cast<const uint8_t *>(&t);
    return std::vector<uint8_t>(p, p + sizeof(T));
}

template <class T>
steam::APICall Call(int32_t callback, const T &result) {
    const steam::APICall call = F().nextCall++;
    F().posted.push_back({call, callback, Bytes(result)});
    return call;
}

template <class T>
void Post(int32_t callback, const T &param) {
    F().posted.push_back({0, callback, Bytes(param)});
}

void Copy(char *to, size_t size, const std::string &from) {
    if (!size) return;
    const size_t n = std::min(from.size(), size - 1);
    std::memcpy(to, from.data(), n);
    to[n] = '\0';
}

std::string Lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool HasTag(const Item &item, const std::string &tag) {
    for (const std::string &t : item.tags)
        if (Lower(t) == Lower(tag)) return true;
    return false;
}

// What a query of everything returns, before paging.
std::vector<int> Matching(const Query &q) {
    const std::vector<Item> &items = F().items;
    std::vector<int> rows;
    for (int i = 0; i < (int)items.size(); ++i) {
        const Item &item = items[i];
        if (!q.text.empty()) {
            std::string all = Lower(item.title + "\n" + item.description);
            for (const std::string &t : item.tags) all += "\n" + Lower(t);
            if (all.find(Lower(q.text)) == std::string::npos) continue;
        }
        if (!q.required.empty()) {
            size_t has = 0;
            for (const std::string &t : q.required) has += HasTag(item, t) ? 1 : 0;
            if (q.matchAny ? has == 0 : has != q.required.size()) continue;
        }
        bool excluded = false;
        for (const std::string &t : q.excluded) excluded = excluded || HasTag(item, t);
        if (excluded) continue;
        rows.push_back(i);
    }
    auto by = [&](auto key) {
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return key(items[a]) > key(items[b]); });
    };
    switch (q.ranking) {
    case steam::kRankedByTrend: by([](const Item &i) { return (double)i.trend; }); break;
    case steam::kRankedByPublicationDate: by([](const Item &i) { return (double)i.timeCreated; }); break;
    case steam::kRankedByTotalUniqueSubscriptions: by([](const Item &i) { return (double)i.subscriptions; }); break;
    case steam::kRankedByVote: by([](const Item &i) { return (double)i.score; }); break;
    default: break;
    }
    return rows;
}

Query *FindQuery(steam::QueryHandle h) {
    const auto it = F().queries.find(h);
    return it == F().queries.end() ? nullptr : &it->second;
}

Update *FindUpdate(steam::UpdateHandle h) {
    const auto it = F().updates.find(h);
    return it == F().updates.end() ? nullptr : &it->second;
}

const Item *Row(steam::QueryHandle h, uint32_t index) {
    const Query *q = FindQuery(h);
    if (!q || index >= q->rows.size() || q->rows[index] < 0) return nullptr;
    return &F().items[(size_t)q->rows[index]];
}

// Ends the downloads and uploads whose time has come.
void Advance() {
    Fake &f = F();
    const unsigned long long now = GetTickCount64();
    for (Item &item : f.items) {
        if (!item.downloadStarted || now - item.downloadStarted < f.downloadMs) continue;
        item.downloadStarted = 0;
        item.installed = true;
        item.needsUpdate = false;
        item.installedAt = ++f.clock;
        if (item.sizeOnDisk == 0) item.sizeOnDisk = item.fileSize;
        Post(steam::kCallDownloadItemResult, steam::DownloadItemResult{f.appid, item.id, steam::kResultOk});
        Post(steam::kCallItemInstalled, steam::ItemInstalled{f.appid, item.id, 0, 0});
    }
    for (auto &[handle, u] : f.updates) {
        if (!u.submitted || now - u.submitted < f.uploadMs) continue;
        u.submitted = 0;
        steam::SubmitItemUpdateResult r{};
        r.id = u.item;
        r.needsLegalAgreement = f.needsLegalAgreement;
        const DWORD a = GetFileAttributesW(FromUtf8(u.content).c_str());
        Item *item = Find(u.item);
        if (const int32_t failure = Failure("submit")) {
            r.result = failure;
        } else if (!item || u.content.empty() || a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            r.result = steam::kResultFileNotFound;
        } else {
            r.result = steam::kResultOk;
            if (u.hasTitle) item->title = u.title;
            if (u.hasDescription) item->description = u.description;
            if (u.hasVisibility) item->visibility = u.visibility;
            if (u.hasTags) item->tags = u.tags;
            for (int32_t d : u.remove) item->descriptors.erase(std::remove(item->descriptors.begin(), item->descriptors.end(), d), item->descriptors.end());
            for (int32_t d : u.add)
                if (std::find(item->descriptors.begin(), item->descriptors.end(), d) == item->descriptors.end()) item->descriptors.push_back(d);
            if (!u.preview.empty()) item->preview = "https://example.invalid/preview/" + std::to_string(item->id) + ".jpg";
            item->timeUpdated = ++f.clock;
        }
        f.posted.push_back({u.call, steam::kCallSubmitItemUpdateResult, Bytes(r)});
    }
}

int32_t InitCommon(char *err) {
    Load();
    Fake &f = F();
    auto say = [&](const char *text) {
        if (err) Copy(err, steam::kErrMsgMax, text);
    };
    Log("init " + std::to_string(f.appid));
    if (Env(L"ANIMELOGON_FAKE_STEAM").empty()) {
        say("fake Steam: ANIMELOGON_FAKE_STEAM names no fixture");
        return steam::kInitNoSteamClient;
    }
    if (!f.running) {
        say("fake Steam: Steam is not running");
        return steam::kInitNoSteamClient;
    }
    if (f.appid == 0) {
        say("fake Steam: SteamAppId is not set");
        return steam::kInitFailedGeneric;
    }
    if (f.initResult != steam::kInitOk) {
        say("fake Steam: the fixture says no");
        return f.initResult;
    }
    f.initialized = true;
    return steam::kInitOk;
}

// Non-null pointers that stand for the interfaces.
int ugcInterface, userInterface, appsInterface;

}  // namespace

// ---- Starting and stopping ----

FAKE_EXPORT int32_t SteamAPI_InitFlat(char *errMsg) { return InitCommon(errMsg); }
FAKE_EXPORT int32_t SteamInternal_SteamAPI_Init(const char *, char *errMsg) { return InitCommon(errMsg); }
FAKE_EXPORT bool SteamAPI_Init() { return InitCommon(nullptr) == steam::kInitOk; }

FAKE_EXPORT void SteamAPI_Shutdown() {
    Log("shutdown");
    F().initialized = false;
}

FAKE_EXPORT bool SteamAPI_IsSteamRunning() {
    Load();
    return F().running;
}

FAKE_EXPORT steam::Pipe SteamAPI_GetHSteamPipe() { return 1; }
FAKE_EXPORT steam::UGC *SteamAPI_SteamUGC_v021() { return reinterpret_cast<steam::UGC *>(&ugcInterface); }
FAKE_EXPORT steam::Users *SteamAPI_SteamUser_v023() { return reinterpret_cast<steam::Users *>(&userInterface); }
FAKE_EXPORT steam::Apps *SteamAPI_SteamApps_v008() { return reinterpret_cast<steam::Apps *>(&appsInterface); }
FAKE_EXPORT bool SteamAPI_ISteamUser_BLoggedOn(steam::Users *) { return F().loggedOn; }
FAKE_EXPORT bool SteamAPI_ISteamApps_BIsSubscribed(steam::Apps *) { return F().owns; }

// ---- Manual dispatch ----

FAKE_EXPORT void SteamAPI_ManualDispatch_Init() { Log("manual dispatch"); }

FAKE_EXPORT void SteamAPI_ManualDispatch_RunFrame(steam::Pipe) {
    Fake &f = F();
    Advance();
    std::vector<Message> posted;
    posted.swap(f.posted);
    for (Message &m : posted) {
        if (!m.call) {
            f.ready.push_back(std::move(m));
            continue;
        }
        const steam::APICallCompleted done{m.call, m.callback, (uint32_t)m.bytes.size()};
        f.ready.push_back({0, steam::kCallAPICallCompleted, Bytes(done)});
        f.results[m.call] = std::move(m);
    }
}

FAKE_EXPORT bool SteamAPI_ManualDispatch_GetNextCallback(steam::Pipe, steam::CallbackMsg *msg) {
    Fake &f = F();
    if (f.ready.empty()) return false;
    f.current = std::move(f.ready.front());
    f.ready.pop_front();
    msg->user = 1;
    msg->callback = f.current.callback;
    msg->param = f.current.bytes.data();
    msg->paramSize = (int32_t)f.current.bytes.size();
    return true;
}

FAKE_EXPORT void SteamAPI_ManualDispatch_FreeLastCallback(steam::Pipe) { F().current = {}; }

FAKE_EXPORT bool SteamAPI_ManualDispatch_GetAPICallResult(steam::Pipe, steam::APICall call, void *result, int32_t resultSize,
                                                          int32_t expectedCallback, bool *failed) {
    Fake &f = F();
    const auto it = f.results.find(call);
    if (it == f.results.end()) return false;
    const Message m = std::move(it->second);
    f.results.erase(it);
    // As Steam does, only into a buffer of the result's own size, for the callback it is.
    if (m.callback != expectedCallback || resultSize != (int32_t)m.bytes.size()) {
        *failed = true;
        return false;
    }
    std::memcpy(result, m.bytes.data(), m.bytes.size());
    *failed = false;
    return true;
}

// ---- Queries ----

FAKE_EXPORT steam::QueryHandle SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage(steam::UGC *, int32_t ranking, int32_t,
                                                                              steam::AppId, steam::AppId, uint32_t page) {
    Query q;
    q.ranking = ranking;
    q.page = page;
    F().queries[F().nextQuery] = q;
    return F().nextQuery++;
}

FAKE_EXPORT steam::QueryHandle SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor(steam::UGC *, int32_t ranking, int32_t,
                                                                                steam::AppId, steam::AppId, const char *cursor) {
    Query q;
    q.ranking = ranking;
    q.byCursor = true;
    q.cursor = cursor ? cursor : "";
    F().queries[F().nextQuery] = q;
    return F().nextQuery++;
}

FAKE_EXPORT steam::QueryHandle SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(steam::UGC *, steam::PublishedFileId *ids,
                                                                              uint32_t count) {
    Query q;
    q.details = true;
    q.ids.assign(ids, ids + count);
    F().queries[F().nextQuery] = q;
    return F().nextQuery++;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetRankedByTrendDays(steam::UGC *, steam::QueryHandle h, uint32_t) { return FindQuery(h) != nullptr; }
FAKE_EXPORT bool SteamAPI_ISteamUGC_SetSearchText(steam::UGC *, steam::QueryHandle h, const char *text) {
    Query *q = FindQuery(h);
    if (q) q->text = text ? text : "";
    return q != nullptr;
}
FAKE_EXPORT bool SteamAPI_ISteamUGC_AddRequiredTag(steam::UGC *, steam::QueryHandle h, const char *tag) {
    Query *q = FindQuery(h);
    if (q) q->required.push_back(tag);
    return q != nullptr;
}
FAKE_EXPORT bool SteamAPI_ISteamUGC_AddExcludedTag(steam::UGC *, steam::QueryHandle h, const char *tag) {
    Query *q = FindQuery(h);
    if (q) q->excluded.push_back(tag);
    return q != nullptr;
}
FAKE_EXPORT bool SteamAPI_ISteamUGC_SetMatchAnyTag(steam::UGC *, steam::QueryHandle h, bool any) {
    Query *q = FindQuery(h);
    if (q) q->matchAny = any;
    return q != nullptr;
}
FAKE_EXPORT bool SteamAPI_ISteamUGC_SetReturnLongDescription(steam::UGC *, steam::QueryHandle h, bool longDescription) {
    Query *q = FindQuery(h);
    if (q) q->longDescription = longDescription;
    return q != nullptr;
}
FAKE_EXPORT bool SteamAPI_ISteamUGC_SetReturnAdditionalPreviews(steam::UGC *, steam::QueryHandle h, bool previews) {
    Query *q = FindQuery(h);
    if (q) q->additionalPreviews = previews;
    return q != nullptr;
}

FAKE_EXPORT steam::APICall SteamAPI_ISteamUGC_SendQueryUGCRequest(steam::UGC *, steam::QueryHandle h) {
    Query *q = FindQuery(h);
    if (!q) return steam::kInvalidCall;
    steam::QueryCompleted done{};
    done.handle = h;
    if (const int32_t failure = Failure(q->details ? "details" : "query")) {
        done.result = failure;
        return Call(steam::kCallQueryCompleted, done);
    }
    done.result = steam::kResultOk;
    if (q->details) {
        for (steam::PublishedFileId id : q->ids) {
            const Item *item = Find(id);
            q->rows.push_back(item ? (int)(item - F().items.data()) : -1);
            q->rowIds.push_back(id);
        }
        done.total = (uint32_t)q->rows.size();
    } else {
        const std::vector<int> all = Matching(*q);
        size_t offset = (size_t)(q->page ? q->page - 1 : 0) * steam::kResultsPerPage;
        // The fake's cursors are "o<offset>"; "*" and anything else start at the beginning.
        if (q->byCursor) offset = q->cursor.size() > 1 && q->cursor[0] == 'o' ? (size_t)ParseId(FromUtf8(q->cursor.substr(1))) : 0;
        for (size_t i = offset; i < all.size() && i < offset + steam::kResultsPerPage; ++i) {
            q->rows.push_back(all[i]);
            q->rowIds.push_back(F().items[(size_t)all[i]].id);
        }
        done.total = (uint32_t)all.size();
        // Like Steam, a cursor even at the end: the same one again.
        const size_t next = offset + q->rows.size();
        Copy(done.nextCursor, sizeof(done.nextCursor), next < all.size() ? "o" + std::to_string(next) : q->cursor);
    }
    done.returned = (uint32_t)q->rows.size();
    return Call(steam::kCallQueryCompleted, done);
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetQueryUGCResult(steam::UGC *, steam::QueryHandle h, uint32_t index, steam::Details *d) {
    const Query *q = FindQuery(h);
    if (!q || index >= q->rows.size()) return false;
    std::memset(d, 0, sizeof(*d));
    d->id = q->rowIds[index];
    const Item *item = Row(h, index);
    if (!item) {
        d->result = steam::kResultFileNotFound;
        return true;
    }
    d->result = steam::kResultOk;
    d->creatorApp = d->consumerApp = F().appid;
    Copy(d->title, sizeof(d->title), item->title);
    // Without the long description Steam cuts it at 255 bytes, in the middle of a character if need be.
    Copy(d->description, q->longDescription ? sizeof(d->description) : 256, item->description);
    d->owner = item->owner;
    d->timeCreated = item->timeCreated;
    d->timeUpdated = item->timeUpdated;
    d->visibility = item->visibility;
    std::string tags;
    for (const std::string &t : item->tags) tags += (tags.empty() ? "" : ",") + t;
    Copy(d->tags, sizeof(d->tags), tags);
    d->fileSize = (int32_t)std::min<uint64_t>(item->fileSize, INT32_MAX);
    d->totalFilesSize = item->fileSize;
    d->votesUp = item->votesUp;
    d->votesDown = item->votesDown;
    d->score = item->score;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetQueryUGCPreviewURL(steam::UGC *, steam::QueryHandle h, uint32_t index, char *url, uint32_t size) {
    const Item *item = Row(h, index);
    if (!item) return false;
    Copy(url, size, item->preview);
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetQueryUGCStatistic(steam::UGC *, steam::QueryHandle h, uint32_t index, int32_t statistic,
                                                         uint64_t *value) {
    const Item *item = Row(h, index);
    if (!item) return false;
    *value = statistic == steam::kStatSubscriptions ? item->subscriptions : statistic == steam::kStatFavorites ? item->favorites : 0;
    return true;
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetQueryUGCNumTags(steam::UGC *, steam::QueryHandle h, uint32_t index) {
    const Item *item = Row(h, index);
    return item ? (uint32_t)item->tags.size() : 0;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetQueryUGCTag(steam::UGC *, steam::QueryHandle h, uint32_t index, uint32_t tag, char *value,
                                                   uint32_t size) {
    const Item *item = Row(h, index);
    if (!item || tag >= item->tags.size()) return false;
    Copy(value, size, item->tags[tag]);
    return true;
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors(steam::UGC *, steam::QueryHandle h, uint32_t index,
                                                                      int32_t *descriptors, uint32_t maxCount) {
    const Item *item = Row(h, index);
    if (!item) return 0;
    const uint32_t n = std::min<uint32_t>((uint32_t)item->descriptors.size(), maxCount);
    std::copy(item->descriptors.begin(), item->descriptors.begin() + n, descriptors);
    return n;
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews(steam::UGC *, steam::QueryHandle h, uint32_t index) {
    const Query *q = FindQuery(h);
    const Item *item = Row(h, index);
    return item && q->additionalPreviews ? (uint32_t)item->previews.size() : 0;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview(steam::UGC *, steam::QueryHandle h, uint32_t index, uint32_t preview,
                                                                 char *url, uint32_t urlSize, char *name, uint32_t nameSize,
                                                                 int32_t *type) {
    const Query *q = FindQuery(h);
    const Item *item = Row(h, index);
    if (!item || !q->additionalPreviews || preview >= item->previews.size()) return false;
    Copy(url, urlSize, item->previews[preview].url);
    Copy(name, nameSize, item->previews[preview].name);
    *type = item->previews[preview].type;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(steam::UGC *, steam::QueryHandle h) { return F().queries.erase(h) == 1; }

// ---- Subscriptions and downloads ----

FAKE_EXPORT steam::APICall SteamAPI_ISteamUGC_SubscribeItem(steam::UGC *, steam::PublishedFileId id) {
    Item *item = Find(id);
    steam::SubscriptionResult r{steam::kResultOk, id};
    if (const int32_t failure = Failure("subscribe"))
        r.result = failure;
    else if (!item)
        r.result = steam::kResultFileNotFound;
    else {
        item->subscribed = true;
        Post(steam::kCallSubscribedItemsChanged, steam::SubscribedItemsChanged{F().appid});
    }
    return Call(steam::kCallSubscribeResult, r);
}

FAKE_EXPORT steam::APICall SteamAPI_ISteamUGC_UnsubscribeItem(steam::UGC *, steam::PublishedFileId id) {
    Item *item = Find(id);
    steam::SubscriptionResult r{steam::kResultOk, id};
    if (const int32_t failure = Failure("unsubscribe"))
        r.result = failure;
    else if (!item)
        r.result = steam::kResultFileNotFound;
    else {
        item->subscribed = false;
        Post(steam::kCallSubscribedItemsChanged, steam::SubscribedItemsChanged{F().appid});
    }
    return Call(steam::kCallUnsubscribeResult, r);
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetNumSubscribedItems(steam::UGC *, bool) {
    uint32_t n = 0;
    for (const Item &i : F().items) n += i.subscribed ? 1 : 0;
    return n;
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetSubscribedItems(steam::UGC *, steam::PublishedFileId *ids, uint32_t maxCount, bool) {
    uint32_t n = 0;
    for (const Item &i : F().items)
        if (i.subscribed && n < maxCount) ids[n++] = i.id;
    return n;
}

FAKE_EXPORT uint32_t SteamAPI_ISteamUGC_GetItemState(steam::UGC *, steam::PublishedFileId id) {
    const Item *item = Find(id);
    if (!item) return 0;
    return (item->subscribed ? steam::kStateSubscribed : 0) | (item->installed ? steam::kStateInstalled : 0) |
           (item->needsUpdate ? steam::kStateNeedsUpdate : 0) | (item->downloadStarted ? steam::kStateDownloading : 0);
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetItemInstallInfo(steam::UGC *, steam::PublishedFileId id, uint64_t *sizeOnDisk, char *folder,
                                                       uint32_t folderSize, uint32_t *timeStamp) {
    const Item *item = Find(id);
    if (!item || !item->installed) return false;
    *sizeOnDisk = item->sizeOnDisk;
    Copy(folder, folderSize, ToUtf8(FolderOf(*item)));
    *timeStamp = item->installedAt;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_GetItemDownloadInfo(steam::UGC *, steam::PublishedFileId id, uint64_t *downloaded, uint64_t *total) {
    const Item *item = Find(id);
    if (!item || !item->downloadStarted) return false;
    const uint64_t size = item->fileSize ? item->fileSize : 1000;
    const unsigned long long elapsed = GetTickCount64() - item->downloadStarted;
    *total = size;
    *downloaded = F().downloadMs ? std::min<uint64_t>(size, size * elapsed / F().downloadMs) : size;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_DownloadItem(steam::UGC *, steam::PublishedFileId id, bool highPriority) {
    Log("download " + std::to_string(id) + (highPriority ? " high" : " normal"));
    Item *item = Find(id);
    if (!item) return false;
    if (const int32_t failure = Failure("download")) {
        Post(steam::kCallDownloadItemResult, steam::DownloadItemResult{F().appid, id, failure});
        return true;
    }
    // Up to date already: nothing to fetch, the result comes at once and the files keep their time.
    if (item->installed && !item->needsUpdate) {
        Post(steam::kCallDownloadItemResult, steam::DownloadItemResult{F().appid, id, steam::kResultOk});
        return true;
    }
    item->downloadStarted = GetTickCount64();
    return true;
}

// ---- Publishing ----

FAKE_EXPORT steam::APICall SteamAPI_ISteamUGC_CreateItem(steam::UGC *, steam::AppId app, int32_t fileType) {
    Fake &f = F();
    Log("create " + std::to_string(app) + " " + std::to_string(fileType));
    steam::CreateItemResult r{};
    r.needsLegalAgreement = f.needsLegalAgreement;
    if (const int32_t failure = Failure("create")) {
        r.result = failure;
    } else {
        Item item;
        item.id = f.nextItem++;
        item.owner = 76561198000000000ull;
        item.timeCreated = item.timeUpdated = ++f.clock;
        item.visibility = steam::kVisibilityPrivate;
        f.items.push_back(item);
        r.result = steam::kResultOk;
        r.id = item.id;
    }
    return Call(steam::kCallCreateItemResult, r);
}

FAKE_EXPORT steam::UpdateHandle SteamAPI_ISteamUGC_StartItemUpdate(steam::UGC *, steam::AppId, steam::PublishedFileId id) {
    Log("update " + std::to_string(id));
    if (!Find(id)) return steam::kInvalidUpdate;
    Update u;
    u.item = id;
    F().updates[F().nextUpdate] = u;
    return F().nextUpdate++;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemTitle(steam::UGC *, steam::UpdateHandle h, const char *title) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log(std::string("title ") + title);
    u->hasTitle = true;
    u->title = title;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemDescription(steam::UGC *, steam::UpdateHandle h, const char *description) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log(std::string("description ") + description);
    u->hasDescription = true;
    u->description = description;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemVisibility(steam::UGC *, steam::UpdateHandle h, int32_t visibility) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log("visibility " + std::to_string(visibility));
    u->hasVisibility = true;
    u->visibility = visibility;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemTags(steam::UGC *, steam::UpdateHandle h, const steam::StringArray *tags, bool) {
    Update *u = FindUpdate(h);
    if (!u || !tags) return false;
    u->hasTags = true;
    u->tags.clear();
    std::string line = "tags";
    for (int32_t i = 0; i < tags->count; ++i) {
        u->tags.push_back(tags->strings[i]);
        line += std::string(i ? "," : " ") + tags->strings[i];
    }
    Log(line);
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_AddContentDescriptor(steam::UGC *, steam::UpdateHandle h, int32_t descriptor) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log("descriptor+ " + std::to_string(descriptor));
    u->add.push_back(descriptor);
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_RemoveContentDescriptor(steam::UGC *, steam::UpdateHandle h, int32_t descriptor) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log("descriptor- " + std::to_string(descriptor));
    u->remove.push_back(descriptor);
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemContent(steam::UGC *, steam::UpdateHandle h, const char *folder) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log(std::string("content ") + folder);
    u->content = folder;
    return true;
}

FAKE_EXPORT bool SteamAPI_ISteamUGC_SetItemPreview(steam::UGC *, steam::UpdateHandle h, const char *file) {
    Update *u = FindUpdate(h);
    if (!u) return false;
    Log(std::string("preview ") + file);
    u->preview = file;
    return true;
}

FAKE_EXPORT steam::APICall SteamAPI_ISteamUGC_SubmitItemUpdate(steam::UGC *, steam::UpdateHandle h, const char *changeNote) {
    Update *u = FindUpdate(h);
    if (!u || u->submitted) return steam::kInvalidCall;
    Log(std::string("submit ") + (changeNote ? changeNote : ""));
    u->submitted = GetTickCount64();
    u->call = F().nextCall++;
    return u->call;
}

FAKE_EXPORT int32_t SteamAPI_ISteamUGC_GetItemUpdateProgress(steam::UGC *, steam::UpdateHandle h, uint64_t *processed, uint64_t *total) {
    const Update *u = FindUpdate(h);
    *processed = *total = 0;
    if (!u || !u->submitted) return steam::kUpdateInvalid;
    const unsigned long long elapsed = GetTickCount64() - u->submitted, whole = std::max(1u, F().uploadMs);
    if (elapsed < whole / 5) return steam::kUpdatePreparingConfig;
    if (elapsed >= whole * 4 / 5) return steam::kUpdateCommittingChanges;
    *total = 1000000;
    *processed = *total * elapsed / whole;
    return steam::kUpdateUploadingContent;
}

// Every entry point the helper looks up is here, with the type it calls it by.
#define FAKE_CHECK(name, type) static_assert(std::is_same_v<decltype(name), type>, #name " is not as steam_flat.h declares it");
STEAM_FLAT_REQUIRED(FAKE_CHECK)
STEAM_FLAT_OPTIONAL(FAKE_CHECK)
#undef FAKE_CHECK
