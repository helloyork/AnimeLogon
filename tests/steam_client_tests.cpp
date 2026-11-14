// The settings app's client of animelogon-steam.exe (app/src/steam_client.h) against the real
// helper and the stand-in steam_api64.dll (tests/fake_steam), laid out as the Steam edition
// installs them: a steam\ folder beside the program. Answers that come in another order than the
// requests are matched by their ids; the helper exits when the client stops it.
#include "check.h"

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "animelogon/secure.h"
#include "animelogon/text.h"

#include "appid.h"
#include "json.h"
#include "steam_client.h"

using namespace animelogon;

namespace {

const wchar_t kHelperExe[] = L"" ANIMELOGON_STEAM_HELPER;
const wchar_t kFakeDllFile[] = L"" ANIMELOGON_FAKE_STEAM_DLL;

std::wstring Folder() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-steamclient-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteText(const std::wstring &path, const std::string &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    return CloseHandle(h) && ok;
}

// Takes what the client read until `done` says it has everything, or 15 seconds have gone.
template <class Done>
std::vector<json::Value> Collect(app::steam::Client &c, HANDLE woken, Done done, bool *exited = nullptr) {
    std::vector<json::Value> all;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    bool gone = false;
    while (!done(all) && !gone && GetTickCount64() < deadline) {
        WaitForSingleObject(woken, 200);
        for (json::Value &v : c.Take(&gone)) all.push_back(std::move(v));
    }
    if (exited) *exited = gone;
    return all;
}

}  // namespace

TEST(SteamClientFindsTheHelperOfTheSteamEdition) {
    const std::wstring dir = Folder();
    CHECK(app::steam::HelperIn(dir).empty());
    CreateDirectoryW((dir + L"\\steam").c_str(), nullptr);
    CHECK(app::steam::HelperIn(dir + L"\\").empty());
    CHECK(CopyFileW(kHelperExe, (dir + L"\\steam\\animelogon-steam.exe").c_str(), FALSE));
    CHECK(app::steam::HelperIn(dir) == dir + L"\\steam\\animelogon-steam.exe");
    CHECK(app::steam::HelperIn(dir + L"\\") == dir + L"\\steam\\animelogon-steam.exe");
    CHECK(app::steam::HelperIn(L"").empty());

    // The app id: AnimeLogon's own, unless the environment names another whole number.
    SetEnvironmentVariableW(L"ANIMELOGON_STEAM_APPID", L"480");
    CHECK(app::steam::AppId() == 480);
    SetEnvironmentVariableW(L"ANIMELOGON_STEAM_APPID", L"48x");
    CHECK(app::steam::AppId() == steamhelper::kAppId);
    SetEnvironmentVariableW(L"ANIMELOGON_STEAM_APPID", L"0");
    CHECK(app::steam::AppId() == steamhelper::kAppId);
    SetEnvironmentVariableW(L"ANIMELOGON_STEAM_APPID", nullptr);
    CHECK(app::steam::AppId() == steamhelper::kAppId);

    // A helper that is not there cannot be asked anything.
    app::steam::Client none(dir + L"\\nothing.exe", 480, nullptr);
    CHECK(!none.Start() && none.Request(L"status") == 0 && !none.Running());
    secure::RemoveTree(dir);
}

TEST(SteamClientMatchesAnswersThatComeOutOfOrder) {
    const std::wstring dir = Folder();
    CreateDirectoryW((dir + L"\\steam").c_str(), nullptr);
    CHECK(CopyFileW(kHelperExe, (dir + L"\\steam\\animelogon-steam.exe").c_str(), FALSE));
    CHECK(CopyFileW(kFakeDllFile, (dir + L"\\steam\\steam_api64.dll").c_str(), FALSE));
    CreateDirectoryW((dir + L"\\upload").c_str(), nullptr);
    CHECK(WriteText(dir + L"\\upload\\theme.altheme", "package"));
    CHECK(WriteText(dir + L"\\fixture.json",
                    R"({"running":true,"uploadMs":900,"downloadMs":300,"nextItemId":"4000000000","items":[
                       {"id":"3100000001","title":"Night","tags":["Video","Everyone"],"subscriptions":5,"fileSize":10},
                       {"id":"3100000002","title":"Library","tags":["Picture","Everyone"],"subscriptions":9,"fileSize":20}]})"));
    // The helper takes the environment the settings app runs in.
    SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM", (dir + L"\\fixture.json").c_str());
    HANDLE woken = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        app::steam::Client c(app::steam::HelperIn(dir), 480, [woken] { SetEvent(woken); });
        CHECK(!c.Running());
        // An upload takes the stand-in 900 ms; Steam's state is said at once.
        const long long publish = c.Request(L"publish", [&dir](json::Writer &w) {
            w.Member(L"folder", dir + L"\\upload").Member(L"title", L"Mine");
        });
        CHECK(c.Running());
        const long long status = c.Request(L"status");
        const long long query = c.Request(L"query", [](json::Writer &w) {
            w.Member(L"sort", L"subscribed").Key(L"requiredTags").BeginArray().String(L"Picture").EndArray();
        });
        CHECK(publish > 0 && status > publish && query > status);

        std::map<long long, size_t> at;  // request id -> where its answer came
        const std::vector<json::Value> lines = Collect(c, woken, [&](const std::vector<json::Value> &all) {
            at.clear();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i][L"id"].IsNumber()) at[(long long)all[i][L"id"].Num()] = i;
            return at.count(publish) && at.count(status) && at.count(query);
        });
        CHECK(at.count(publish) && at.count(status) && at.count(query));
        if (at.count(publish) && at.count(status) && at.count(query)) {
            CHECK(at[status] < at[publish] && at[query] < at[publish]);
            CHECK(lines[at[status]][L"ok"].Bool() && lines[at[status]][L"steam"].Str() == L"ok");
            const json::Value &q = lines[at[query]];
            CHECK(q[L"ok"].Bool() && q[L"items"].size() == 1 && q[L"items"][0][L"id"].Str() == L"3100000002");
            CHECK(lines[at[publish]][L"ok"].Bool() && lines[at[publish]][L"itemId"].Str() == L"4000000000");
        }
        // The upload's progress came between, as events of that request.
        bool progressed = false;
        for (const json::Value &v : lines)
            progressed = progressed || (v[L"event"].Str() == L"progress" && (long long)v[L"request"].Num() == publish);
        CHECK(progressed);

        // A download answers at once and is installed later, by an event.
        const long long download = c.Request(L"download", [](json::Writer &w) { w.Member(L"itemId", L"3100000001"); });
        const std::vector<json::Value> later = Collect(c, woken, [&](const std::vector<json::Value> &all) {
            for (const json::Value &v : all)
                if (v[L"event"].Str() == L"installed" && v[L"id"].Str() == L"3100000001") return true;
            return false;
        });
        bool answered = false, installed = false;
        for (const json::Value &v : later) {
            answered = answered || ((long long)v[L"id"].Num(-1) == download && v[L"ok"].Bool());
            installed = installed || v[L"event"].Str() == L"installed";
        }
        CHECK(answered && installed);

        // Stopped, it exits; asked again, it starts again.
        c.Stop();
        CHECK(!c.Running());
        const long long again = c.Request(L"status");
        CHECK(again > 0 && c.Running());
        const std::vector<json::Value> more = Collect(c, woken, [&](const std::vector<json::Value> &all) {
            for (const json::Value &v : all)
                if ((long long)v[L"id"].Num(-1) == again) return true;
            return false;
        });
        CHECK(!more.empty());
    }
    SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM", nullptr);
    CloseHandle(woken);
    secure::RemoveTree(dir);
}

TEST(SteamClientNoticesTheHelperGoing) {
    const std::wstring dir = Folder();
    CreateDirectoryW((dir + L"\\steam").c_str(), nullptr);
    CHECK(CopyFileW(kHelperExe, (dir + L"\\steam\\animelogon-steam.exe").c_str(), FALSE));
    HANDLE woken = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        // Without Steam's library the helper still answers, saying so.
        app::steam::Client c(app::steam::HelperIn(dir), 480, [woken] { SetEvent(woken); });
        const long long status = c.Request(L"status");
        std::vector<json::Value> lines = Collect(c, woken, [&](const std::vector<json::Value> &all) { return !all.empty(); });
        CHECK(lines.size() == 1 && (long long)lines[0][L"id"].Num() == status && lines[0][L"steam"].Str() == L"missing_dll");
        // Asked to quit by the protocol rather than by Stop: it goes, and the client says so.
        const long long quit = c.Request(L"quit");
        bool exited = false;
        lines = Collect(c, woken, [](const std::vector<json::Value> &) { return false; }, &exited);
        CHECK(exited && !c.Running());
        CHECK(lines.size() == 1 && (long long)lines[0][L"id"].Num() == quit);
    }
    CloseHandle(woken);
    secure::RemoveTree(dir);
}
