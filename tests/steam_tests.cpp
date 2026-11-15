// animelogon-steam.exe (steam/) run as the settings app runs it, a child process spoken to in JSON
// lines (steam/PROTOCOL.md), against the stand-in steam_api64.dll (tests/fake_steam) and fixtures
// the tests write. Each test copies the helper, and the stand-in when it wants one, into a scratch
// folder: the helper loads the library from its own folder only.
#include "check.h"

#include <windows.h>

#include <string>
#include <vector>

#include "animelogon/secure.h"
#include "animelogon/text.h"

#include "json.h"

using namespace animelogon;

namespace {

constexpr DWORD kWaitMs = 15000;

const wchar_t kHelper[] = L"" ANIMELOGON_STEAM_HELPER;
const wchar_t kFakeDll[] = L"" ANIMELOGON_FAKE_STEAM_DLL;

std::wstring Scratch() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-steam-" + RandomHex(6);
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

std::string ReadText(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(path, &bytes, 1 << 20)) return "";
    return std::string(bytes.begin(), bytes.end());
}

std::string Q(const std::string &text) { return "\"" + text + "\""; }

// A JSON string for a Windows path.
std::string QPath(const std::wstring &path) {
    std::string out = "\"";
    for (char c : ToUtf8(path)) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out + "\"";
}

// A scratch folder with the helper and, unless asked otherwise, the stand-in library; removed after.
struct Lab {
    std::wstring dir;
    explicit Lab(bool dll = true) : dir(Scratch()) {
        CHECK(CopyFileW(kHelper, (dir + L"\\animelogon-steam.exe").c_str(), FALSE));
        if (dll) CHECK(CopyFileW(kFakeDll, (dir + L"\\steam_api64.dll").c_str(), FALSE));
    }
    ~Lab() { secure::RemoveTree(dir); }
    std::wstring Fixture(const std::string &json) const {
        const std::wstring path = dir + L"\\fixture.json";
        CHECK(WriteText(path, json));
        return path;
    }
    std::wstring Log() const { return dir + L"\\calls.log"; }
};

// The helper as a child process, with pipes for its standard input and output.
class Helper {
public:
    Helper(const Lab &lab, const std::wstring &args, const std::wstring &fixture) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE inRead = nullptr, outWrite = nullptr;
        CHECK(CreatePipe(&inRead, &in_, &sa, 0) && CreatePipe(&out_, &outWrite, &sa, 0));
        SetHandleInformation(in_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
        // The child takes the environment it is started with; the test's own is put back after.
        SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM", fixture.empty() ? nullptr : fixture.c_str());
        SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM_LOG", lab.Log().c_str());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command = L"\"" + lab.dir + L"\\animelogon-steam.exe\" " + args;
        PROCESS_INFORMATION pi{};
        // The scratch folder is not the current directory: the helper must find the library by its own path.
        CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        process_ = pi.hProcess;
        if (pi.hThread) CloseHandle(pi.hThread);
        SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM", nullptr);
        SetEnvironmentVariableW(L"ANIMELOGON_FAKE_STEAM_LOG", nullptr);
        CloseHandle(inRead);
        CloseHandle(outWrite);
    }

    ~Helper() {
        if (in_) CloseHandle(in_);
        if (process_) {
            if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
                CHECK(!"the helper did not exit");
                TerminateProcess(process_, 1);
            }
            CloseHandle(process_);
        }
        if (out_) CloseHandle(out_);
    }

    void SendLine(const std::string &line) {
        const std::string bytes = line + "\n";
        DWORD wrote = 0;
        CHECK(WriteFile(in_, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr));
    }

    // Sends {"id":<next>,"op":<op>,<args>} and returns its answer; null after kWaitMs without one.
    json::Value Call(const std::string &op, const std::string &args = "") {
        const long long id = ++last_;
        SendLine("{\"id\":" + std::to_string(id) + ",\"op\":" + Q(op) + (args.empty() ? "" : "," + args) + "}");
        return Answer(id);
    }

    json::Value Answer(long long id) {
        for (;;) {
            json::Value v = Next();
            if (v.IsNull()) return v;
            if (v[L"id"].IsNumber() && (long long)v[L"id"].Num() == id) return v;
            if (v[L"event"].IsString()) events.push_back(v);
        }
    }

    // The first event named `name` (for the item `id`, when given), among those seen or still to come.
    json::Value WaitEvent(const wchar_t *name, const std::wstring &id = L"") {
        for (size_t i = 0;; ++i) {
            if (i == events.size()) {
                json::Value v = Next();
                if (v.IsNull()) return v;
                if (!v[L"event"].IsString()) continue;
                events.push_back(v);
            }
            if (events[i][L"event"].Str() == name && (id.empty() || events[i][L"id"].Str() == id)) return events[i];
        }
    }

    size_t Count(const wchar_t *name, const wchar_t *op) const {
        size_t n = 0;
        for (const json::Value &e : events) n += e[L"event"].Str() == name && e[L"op"].Str() == op ? 1 : 0;
        return n;
    }

    // Closes standard input and returns the exit code, or -1 when the helper is still running.
    long Close() {
        CloseHandle(in_);
        in_ = nullptr;
        return Exit();
    }

    long Exit() {
        DWORD code = 0;
        if (WaitForSingleObject(process_, kWaitMs) != WAIT_OBJECT_0 || !GetExitCodeProcess(process_, &code)) return -1;
        return (long)code;
    }

    std::vector<json::Value> events;

    // The next line, parsed; null after kWaitMs without one or when the helper has gone.
    json::Value Next() {
        const ULONGLONG deadline = GetTickCount64() + kWaitMs;
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos) {
                const std::string line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                json::Value v;
                CHECK(json::Parse(FromUtf8(line), &v) && v.IsObject());
                return v;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &available, nullptr)) return {};
            if (available) {
                std::string chunk(available, '\0');
                DWORD got = 0;
                if (!ReadFile(out_, chunk.data(), available, &got, nullptr)) return {};
                buffer_.append(chunk.data(), got);
                continue;
            }
            if (GetTickCount64() > deadline) return {};
            Sleep(5);
        }
    }

private:
    HANDLE in_ = nullptr, out_ = nullptr, process_ = nullptr;
    std::string buffer_;
    long long last_ = 0;
};

bool Ok(const json::Value &v) { return v[L"ok"].Bool(false); }
std::wstring Error(const json::Value &v) { return v[L"ok"].IsBool() && !v[L"ok"].Bool() ? v[L"error"].Str() : L"(none)"; }

std::vector<std::wstring> Ids(const json::Value &answer) {
    std::vector<std::wstring> ids;
    for (const json::Value &item : answer[L"items"].Items()) ids.push_back(item[L"id"].Str());
    return ids;
}

std::wstring ItemId(int i) { return std::to_wstring(3000000000ll + i); }

// A query's parameter showing every age rating, mature content included.
const std::string kAllRatings = "\"ratings\":[\"Everyone\",\"Questionable\",\"Mature\"]";

// Sixty items whose orders differ by every ranking: by trend item 59 comes first, by date 17, by
// subscriptions 49, by rating 23. Items 3 and 8 mention sakura; 3 and 4 have mature content (3 by
// a content descriptor, 4 by its rating tag), 5 is rated Questionable, 6 has no rating, the others
// are rated Everyone.
std::string SixtyItems() {
    std::string items;
    for (int i = 0; i < 60; ++i) {
        std::string tags = i % 2 ? "[\"Video\"" : "[\"Picture\"";
        tags += i % 3 == 0 ? ",\"4K\"" : ",\"1080p\"";
        tags += i == 4 ? ",\"Mature\"]" : i == 5 ? ",\"Questionable\"]" : i == 6 ? "]" : ",\"Everyone\"]";
        std::string title = "Item " + std::to_string(i), description = "plain";
        std::string descriptors = "[]";
        if (i == 3) title = "Sakura night", descriptors = "[1]";
        if (i == 4) descriptors = "[5]";
        if (i == 8) description = "petals of a SAKURA tree";
        if (i == 9) {
            // 300 two-byte characters: Steam's short description cuts the 128th in half.
            description.clear();
            for (int k = 0; k < 300; ++k) description += "\xC3\xA9";
        }
        if (!items.empty()) items += ",";
        items += "{\"id\":" + Q(ToUtf8(ItemId(i))) + ",\"title\":" + Q(title) + ",\"description\":" + Q(description) +
                 ",\"tags\":" + tags + ",\"descriptors\":" + descriptors + ",\"trend\":" + std::to_string(i) +
                 ",\"timeCreated\":" + std::to_string(1600000000 + (i * 7 % 60) * 1000) +
                 ",\"subscriptions\":" + std::to_string((i * 11 % 60) * 10) + ",\"score\":" + std::to_string((i * 13 % 60) / 60.0) +
                 ",\"votesUp\":" + std::to_string(i) + ",\"votesDown\":1,\"favorites\":2,\"fileSize\":" + std::to_string(1000 + i) +
                 ",\"owner\":\"76561198000000042\",\"preview\":\"https://example.invalid/" + std::to_string(i) + ".jpg\"" +
                 ",\"previews\":[{\"url\":\"https://example.invalid/more.jpg\",\"type\":0,\"name\":\"more.jpg\"},"
                 "{\"url\":\"dQw4w9WgXcQ\",\"type\":1}]}";
    }
    return "{\"items\":[" + items + "]}";
}

std::string ThreeItems(const std::string &more = "") {
    return "{" + more + (more.empty() ? "" : ",") +
           "\"downloadMs\":400,\"items\":["
           "{\"id\":\"100\",\"title\":\"Already here\",\"subscribed\":true,\"installed\":true,\"folder\":\"content\\\\100\","
           "\"sizeOnDisk\":5000,\"timeUpdated\":1650000000,\"descriptors\":[\"generalMature\"],\"preview\":\"https://example.invalid/100.jpg\"},"
           "{\"id\":\"200\",\"title\":\"Two hundred\",\"fileSize\":2000000,\"timeUpdated\":1660000000},"
           "{\"id\":\"300\",\"title\":\"Three hundred\"}]}";
}

}  // namespace

TEST(SteamHelperWithoutSteamsLibraryAnswersStatus) {
    Lab lab(false);
    Helper h(lab, L"--appid 480", L"");
    const json::Value s = h.Call("status");
    CHECK(Ok(s) && s[L"steam"].Str() == L"missing_dll" && s[L"protocol"].Num() == 2);
    CHECK(s[L"appid"].Num() == 480 && !s[L"loggedOn"].Bool(true) && !s[L"ownsApp"].Bool(true) && !s[L"message"].Str().empty());
    CHECK(Error(h.Call("query")) == L"missing_dll");
    CHECK(Error(h.Call("subscribed")) == L"missing_dll");
    CHECK(Ok(h.Call("quit")));
    CHECK(h.Exit() == 0);
}

TEST(SteamHelperWithoutAnAppIdDoesNotLoadSteam) {
    Lab lab;
    Helper h(lab, L"", lab.Fixture("{\"items\":[]}"));
    const json::Value s = h.Call("status");
    CHECK(Ok(s) && s[L"steam"].Str() == L"no_appid" && s[L"appid"].Num() == 0);
    CHECK(Error(h.Call("details", "\"itemId\":\"1\"")) == L"no_appid");
    CHECK(h.Close() == 0);
    CHECK(ReadText(lab.Log()).empty());  // the library was never asked anything
}

TEST(SteamHelperTellsSteamNotRunningFromOtherFailures) {
    {
        Lab lab;
        Helper h(lab, L"--appid 480", lab.Fixture("{\"running\":false}"));
        const json::Value s = h.Call("status");
        CHECK(Ok(s) && s[L"steam"].Str() == L"not_running" && !s[L"message"].Str().empty());
        CHECK(Error(h.Call("query")) == L"not_running");
        CHECK(h.Close() == 0);
    }
    {
        Lab lab;
        Helper h(lab, L"--appid 480", lab.Fixture("{\"initResult\":3}"));
        const json::Value s = h.Call("status");
        CHECK(Ok(s) && s[L"steam"].Str() == L"init_failed" && !s[L"message"].Str().empty());
        CHECK(h.Close() == 0);
    }
}

TEST(SteamHelperRefusesAnAccountThatDoesNotOwnTheApp) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture("{\"owns\":false,\"items\":[{\"id\":\"1\",\"title\":\"a\"}]}"));
    const json::Value s = h.Call("status");
    CHECK(Ok(s) && s[L"steam"].Str() == L"ok" && s[L"loggedOn"].Bool() && !s[L"ownsApp"].Bool(true));
    CHECK(Error(h.Call("query")) == L"not_owned");
    CHECK(Error(h.Call("subscribe", "\"itemId\":\"1\"")) == L"not_owned");
    CHECK(Ok(h.Call("quit")));
    CHECK(h.Exit() == 0);
    // Disconnected from Steam before it exited.
    const std::string log = ReadText(lab.Log());
    CHECK(log.find("init 480") != std::string::npos && log.find("shutdown") != std::string::npos);
}

TEST(SteamHelperAnswersBadRequests) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture("{\"items\":[]}"));
    // A line that is not a request is answered without an id.
    h.SendLine("not json");
    const json::Value v = h.Next();
    CHECK(Error(v) == L"bad_request" && v[L"id"].IsNull());
    h.SendLine("{\"op\":\"status\"}");
    CHECK(Error(h.Next()) == L"bad_request");
    h.SendLine("{\"id\":1000,\"op\":\"dance\"}");
    CHECK(Error(h.Answer(1000)) == L"unknown_op");
    CHECK(Error(h.Call("query", "\"sort\":\"best\"")) == L"bad_request");
    CHECK(Error(h.Call("query", "\"perPage\":51")) == L"bad_request");
    CHECK(Error(h.Call("details", "\"itemId\":42")) == L"bad_request");  // ids are strings
    CHECK(Error(h.Call("subscribe", "\"itemId\":\"12x\"")) == L"bad_request");
    CHECK(h.Close() == 0);
}

TEST(SteamHelperRanksSearchesAndPagesTheWorkshop) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture(SixtyItems()));
    CHECK(h.Call("status")[L"steam"].Str() == L"ok");

    // Trending, Steam's pages of 50: the first one, then the rest.
    json::Value a = h.Call("query", kAllRatings);
    CHECK(Ok(a) && a[L"total"].Num() == 60 && a[L"items"].size() == 50 && a[L"next"].Num() == 2);
    CHECK(Ids(a).size() == 50 && Ids(a)[0] == ItemId(59) && Ids(a)[49] == ItemId(10));
    a = h.Call("query", kAllRatings + ",\"page\":2");
    CHECK(a[L"items"].size() == 10 && a[L"next"].IsNull() && Ids(a)[9] == ItemId(0));

    // Pages of 20: the third takes the end of Steam's first page and all of its second.
    a = h.Call("query", kAllRatings + ",\"perPage\":20,\"page\":3");
    CHECK(a[L"items"].size() == 20 && a[L"next"].IsNull());
    CHECK(Ids(a).size() == 20 && Ids(a)[0] == ItemId(19) && Ids(a)[19] == ItemId(0));
    a = h.Call("query", kAllRatings + ",\"perPage\":20,\"page\":2");
    CHECK(a[L"items"].size() == 20 && a[L"next"].Num() == 3 && Ids(a)[0] == ItemId(39));

    // Each ranking.
    CHECK(Ids(h.Call("query", "\"sort\":\"recent\"," + kAllRatings + ",\"perPage\":1"))[0] == ItemId(17));
    CHECK(Ids(h.Call("query", "\"sort\":\"subscribed\"," + kAllRatings + ",\"perPage\":1"))[0] == ItemId(49));
    CHECK(Ids(h.Call("query", "\"sort\":\"rating\"," + kAllRatings + ",\"perPage\":1"))[0] == ItemId(23));
    CHECK(Ok(h.Call("query", "\"sort\":\"trend\",\"trendDays\":30,\"perPage\":1")));

    // Searching, with only the Everyone rating unless others are asked for.
    a = h.Call("query", "\"text\":\"sakura\"");
    CHECK(a[L"total"].Num() == 2 && Ids(a).size() == 1 && Ids(a)[0] == ItemId(8) && a[L"hidden"].Num() == 1);
    a = h.Call("query", "\"text\":\"sakura\"," + kAllRatings);
    CHECK(Ids(a).size() == 2 && a[L"hidden"].Num() == 0);
    a = h.Call("query", "\"perPage\":10");
    CHECK(Ids(a).size() == 10 && a[L"hidden"].Num() == 0);  // items 3 and 4 are not among the ten trending most

    // Tags: all of them, any of them, none of them.
    a = h.Call("query", "\"requiredTags\":[\"Video\",\"4K\"]," + kAllRatings);
    CHECK(a[L"total"].Num() == 10);  // odd multiples of 3
    a = h.Call("query", "\"requiredTags\":[\"Video\",\"4K\"],\"matchAnyTag\":true," + kAllRatings);
    CHECK(a[L"total"].Num() == 40);
    a = h.Call("query", "\"excludedTags\":[\"Video\"]," + kAllRatings);
    CHECK(a[L"total"].Num() == 30);

    // By cursor: "*" starts, the answer's "next" goes on, null at the end.
    a = h.Call("query", "\"cursor\":\"*\"," + kAllRatings);
    CHECK(a[L"items"].size() == 50 && a[L"next"].IsString() && !a[L"next"].Str().empty());
    a = h.Call("query", "\"cursor\":" + Q(ToUtf8(a[L"next"].Str())) + "," + kAllRatings);
    CHECK(a[L"items"].size() == 10 && a[L"next"].IsNull());

    // One item as a query gives it.
    a = h.Call("query", "\"text\":\"Sakura night\"," + kAllRatings);
    CHECK(a[L"items"].size() == 1);
    const json::Value &item = a[L"items"][0];
    CHECK(item[L"id"].Str() == ItemId(3) && item[L"title"].Str() == L"Sakura night" && item[L"owner"].Str() == L"76561198000000042");
    CHECK(item[L"preview"].Str() == L"https://example.invalid/3.jpg" && item[L"tags"].size() == 3 && item[L"tags"][0].Str() == L"Video");
    CHECK(item[L"descriptors"].size() == 1 && item[L"descriptors"][0].Str() == L"nuditySexual");
    CHECK(item[L"rating"].Str() == L"Mature");  // tagged Everyone, but with a content descriptor
    CHECK(item[L"votesUp"].Num() == 3 && item[L"votesDown"].Num() == 1 && item[L"favorites"].Num() == 2);
    CHECK(item[L"subscriptions"].Num() == 330 && item[L"fileSize"].Num() == 1003);
    CHECK(item[L"timeCreated"].Num() == 1600021000 && !item[L"subscribed"].Bool(true) && !item[L"installed"].Bool(true));
    CHECK(item[L"previews"].IsNull());  // only in details

    // A short description Steam cut inside a character ends before that character.
    a = h.Call("query", "\"text\":\"Item 9\"");
    CHECK(a[L"items"].size() == 1 && a[L"items"][0][L"description"].Str() == std::wstring(127, L'\u00e9'));
    CHECK(h.Close() == 0);
}

// One item for each way an item can be rated: by one rating tag, by none, by a content descriptor
// over an Everyone tag, and by two rating tags.
std::string RatedItems() {
    return "{\"items\":["
           "{\"id\":\"501\",\"title\":\"Everyone\",\"tags\":[\"Picture\",\"Everyone\"],\"trend\":6},"
           "{\"id\":\"502\",\"title\":\"Questionable\",\"tags\":[\"Picture\",\"Questionable\"],\"trend\":5},"
           "{\"id\":\"503\",\"title\":\"Mature\",\"tags\":[\"Video\",\"Mature\"],\"trend\":4},"
           "{\"id\":\"504\",\"title\":\"Unrated\",\"tags\":[\"Video\"],\"trend\":3},"
           "{\"id\":\"505\",\"title\":\"Described\",\"tags\":[\"Video\",\"Everyone\"],\"descriptors\":[\"nuditySexual\"],\"trend\":2},"
           "{\"id\":\"506\",\"title\":\"Both\",\"tags\":[\"Video\",\"Questionable\",\"Mature\"],\"trend\":1}]}";
}

TEST(SteamHelperShowsOnlyTheAgeRatingsAskedFor) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture(RatedItems()));

    // By default, Everyone only: Steam leaves out the items tagged otherwise, the helper the
    // unrated one and the one with a content descriptor.
    json::Value a = h.Call("query");
    CHECK(Ok(a) && a[L"total"].Num() == 3 && Ids(a) == std::vector<std::wstring>{L"501"} && a[L"hidden"].Num() == 2);
    a = h.Call("query", "\"ratings\":[\"Everyone\",\"Questionable\"]");
    CHECK(a[L"total"].Num() == 4 && Ids(a) == (std::vector<std::wstring>{L"501", L"502"}) && a[L"hidden"].Num() == 2);

    // Mature alone: what has no rating tag or a descriptor is Mature; an item with two rating tags
    // shows only when both are asked for.
    a = h.Call("query", "\"ratings\":[\"Mature\"]");
    CHECK(a[L"total"].Num() == 2 && Ids(a) == (std::vector<std::wstring>{L"503", L"504"}) && a[L"hidden"].Num() == 0);

    // Everything, each with its rating.
    a = h.Call("query", kAllRatings);
    CHECK(a[L"total"].Num() == 6 && a[L"items"].size() == 6 && a[L"hidden"].Num() == 0);
    const wchar_t *const expected[] = {L"Everyone", L"Questionable", L"Mature", L"Mature", L"Mature", L"Mature"};
    for (size_t i = 0; i < a[L"items"].size() && i < 6; ++i) CHECK(a[L"items"][i][L"rating"].Str() == expected[i]);

    // Details are not filtered, and say the rating too.
    const json::Value d = h.Call("details", "\"itemId\":\"504\"");
    CHECK(Ok(d) && d[L"item"][L"rating"].Str() == L"Mature");

    CHECK(Error(h.Call("query", "\"ratings\":[]")) == L"bad_request");
    CHECK(Error(h.Call("query", "\"ratings\":[\"Adult\"]")) == L"bad_request");
    CHECK(Error(h.Call("query", "\"ratings\":\"Everyone\"")) == L"bad_request");
    CHECK(Error(h.Call("query", "\"ratings\":[\"everyone\"]")) == L"bad_request");  // the protocol's names, exactly
    CHECK(h.Close() == 0);
}

TEST(SteamHelperGivesAnItemsDetails) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture(SixtyItems()));
    json::Value d = h.Call("details", "\"itemId\":" + Q(ToUtf8(ItemId(9))));
    CHECK(Ok(d) && d[L"item"][L"description"].Str() == std::wstring(300, L'\u00e9'));
    CHECK(d[L"item"][L"visibility"].Str() == L"public");
    const json::Value &previews = d[L"item"][L"previews"];
    CHECK(previews.size() == 2 && previews[0][L"type"].Str() == L"image" && previews[0][L"name"].Str() == L"more.jpg");
    CHECK(previews[1][L"type"].Str() == L"youtube" && previews[1][L"url"].Str() == L"dQw4w9WgXcQ");
    CHECK(Error(h.Call("details", "\"itemId\":\"123\"")) == L"not_found");
    CHECK(h.Close() == 0);
}

TEST(SteamHelperSubscribesAndListsSubscriptions) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture(ThreeItems()));
    json::Value list = h.Call("subscribed");
    CHECK(Ok(list) && list[L"details"].Bool() && list[L"items"].size() == 1);
    const json::Value &here = list[L"items"][0];
    CHECK(here[L"id"].Str() == L"100" && here[L"title"].Str() == L"Already here" && here[L"timeUpdated"].Num() == 1650000000);
    CHECK(here[L"state"][L"subscribed"].Bool() && here[L"state"][L"installed"].Bool() && !here[L"state"][L"needsUpdate"].Bool(true));
    CHECK(here[L"folder"].Str() == lab.dir + L"\\content\\100" && here[L"sizeOnDisk"].Num() == 5000);
    CHECK(here[L"preview"].Str() == L"https://example.invalid/100.jpg" && here[L"descriptors"][0].Str() == L"generalMature");
    CHECK(here[L"rating"].Str() == L"Mature");

    const json::Value s = h.Call("subscribe", "\"itemId\":\"200\"");
    CHECK(Ok(s) && s[L"result"].Num() == 1);
    CHECK(h.WaitEvent(L"subscriptionsChanged")[L"event"].IsString());
    list = h.Call("subscribed");
    CHECK(list[L"items"].size() == 2);
    if (list[L"items"].size() == 2) {
        const json::Value &two = list[L"items"][1];
        CHECK(two[L"id"].Str() == L"200" && two[L"title"].Str() == L"Two hundred" && two[L"state"][L"subscribed"].Bool());
        CHECK(!two[L"state"][L"installed"].Bool(true) && two[L"folder"].IsNull());
    }
    CHECK(Ok(h.Call("unsubscribe", "\"itemId\":\"100\"")));
    list = h.Call("subscribed");
    CHECK(list[L"items"].size() == 1 && list[L"items"][0][L"id"].Str() == L"200");

    const json::Value missing = h.Call("subscribe", "\"itemId\":\"999\"");
    CHECK(Error(missing) == L"steam_error" && missing[L"result"].Num() == 9 && missing[L"message"].Str() == L"FileNotFound");
    CHECK(h.Close() == 0);

    Lab failing;
    Helper f(failing, L"--appid 480", failing.Fixture(ThreeItems("\"fail\":{\"subscribe\":2}")));
    const json::Value refused = f.Call("subscribe", "\"itemId\":\"200\"");
    CHECK(Error(refused) == L"steam_error" && refused[L"result"].Num() == 2);
    CHECK(f.Close() == 0);
}

TEST(SteamHelperReportsADownloadAsItGoes) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture(ThreeItems()));
    CHECK(Ok(h.Call("download", "\"itemId\":\"200\",\"highPriority\":true")));
    const json::Value installed = h.WaitEvent(L"installed", L"200");
    CHECK(installed[L"folder"].Str() == lab.dir + L"\\content\\200" && installed[L"sizeOnDisk"].Num() == 2000000);
    CHECK(h.Count(L"progress", L"download") >= 1);
    // Steam tells of the one install twice (the download's result and the item's installation).
    CHECK(Ok(h.Call("status")));
    size_t installs = 0;
    for (const json::Value &e : h.events) installs += e[L"event"].Str() == L"installed" ? 1 : 0;
    CHECK(installs == 1);
    // Asked again, a download that changes nothing still ends in "installed".
    h.events.clear();
    CHECK(Ok(h.Call("download", "\"itemId\":\"200\"")));
    CHECK(h.WaitEvent(L"installed", L"200")[L"id"].Str() == L"200");
    const json::Value list = h.Call("subscribed");
    CHECK(list[L"items"].size() == 1);  // downloading is not subscribing
    CHECK(Error(h.Call("download", "\"itemId\":\"999\"")) == L"call_failed");
    CHECK(h.Close() == 0);
    CHECK(ReadText(lab.Log()).find("download 200 high") != std::string::npos);

    Lab failing;
    Helper f(failing, L"--appid 480", failing.Fixture(ThreeItems("\"fail\":{\"download\":54}")));
    CHECK(Ok(f.Call("download", "\"itemId\":\"300\"")));
    const json::Value failed = f.WaitEvent(L"downloadFailed", L"300");
    CHECK(failed[L"result"].Num() == 54 && failed[L"message"].Str() == L"DiskFull");
    CHECK(f.Close() == 0);
}

TEST(SteamHelperPublishesAndUpdatesAnItem) {
    Lab lab;
    const std::wstring content = lab.dir + L"\\upload", preview = lab.dir + L"\\preview.jpg";
    CHECK(CreateDirectoryW(content.c_str(), nullptr));
    CHECK(WriteText(content + L"\\theme.altheme", "not really a theme"));
    CHECK(WriteText(preview, "not really a picture"));
    Helper h(lab, L"--appid 480", lab.Fixture("{\"uploadMs\":600,\"items\":[]}"));

    // Requests that cannot be published are refused before Steam is asked.
    CHECK(Error(h.Call("publish", "\"title\":\"x\"")) == L"bad_request");
    CHECK(Error(h.Call("publish", "\"folder\":\"upload\",\"title\":\"x\"")) == L"bad_request");  // not a full path
    CHECK(Error(h.Call("publish", "\"folder\":" + QPath(content))) == L"bad_request");          // a new item needs a title
    CHECK(Error(h.Call("publish", "\"folder\":" + QPath(content) + ",\"title\":\"x\",\"descriptors\":[\"gore\"]")) == L"bad_request");
    CHECK(Error(h.Call("publish", "\"folder\":" + QPath(content) + ",\"title\":\"x\",\"tags\":[\"a,b\"]")) == L"bad_request");
    CHECK(Error(h.Call("publish", "\"folder\":" + QPath(content) + ",\"title\":\"x\",\"visibility\":\"everyone\"")) == L"bad_request");

    const json::Value p = h.Call("publish", "\"folder\":" + QPath(content) + ",\"preview\":" + QPath(preview) +
                                                ",\"title\":\"My theme\",\"description\":\"Line one\\nLine two\","
                                                "\"visibility\":\"public\",\"tags\":[\"Video\",\"4K\"],"
                                                "\"descriptors\":[\"violenceGore\"],\"changeNote\":\"First\"");
    CHECK(Ok(p) && p[L"itemId"].Str() == L"4000000000" && p[L"created"].Bool() && !p[L"needsLegalAgreement"].Bool(true));
    CHECK(h.Count(L"progress", L"publish") >= 1);
    for (const json::Value &e : h.events)
        if (e[L"op"].Str() == L"publish") CHECK(e[L"itemId"].Str() == L"4000000000" && !e[L"stage"].Str().empty());

    json::Value d = h.Call("details", "\"itemId\":\"4000000000\"");
    CHECK(Ok(d) && d[L"item"][L"title"].Str() == L"My theme" && d[L"item"][L"description"].Str() == L"Line one\nLine two");
    CHECK(d[L"item"][L"visibility"].Str() == L"public" && d[L"item"][L"tags"].size() == 2);
    CHECK(d[L"item"][L"descriptors"].size() == 1 && d[L"item"][L"descriptors"][0].Str() == L"violenceGore");

    // An update names the item; the descriptors it lists replace the item's.
    const json::Value u = h.Call("publish", "\"itemId\":\"4000000000\",\"folder\":" + QPath(content) + ",\"descriptors\":[]");
    CHECK(Ok(u) && u[L"itemId"].Str() == L"4000000000" && !u[L"created"].Bool(true));
    d = h.Call("details", "\"itemId\":\"4000000000\"");
    CHECK(d[L"item"][L"title"].Str() == L"My theme" && d[L"item"][L"descriptors"].size() == 0);
    CHECK(h.Close() == 0);
    const std::string log = ReadText(lab.Log());
    CHECK(log.find("create 480 0") != std::string::npos && log.find("tags Video,4K") != std::string::npos);
    CHECK(log.find("descriptor+ 2") != std::string::npos && log.find("descriptor- 2") != std::string::npos);
    CHECK(log.find("visibility 0") != std::string::npos && log.find("submit First") != std::string::npos);
    CHECK(log.find("content " + ToUtf8(content)) != std::string::npos && log.find("shutdown") != std::string::npos);
}

TEST(SteamHelperSaysWhenTheWorkshopAgreementIsNeeded) {
    Lab lab;
    const std::wstring content = lab.dir + L"\\upload";
    CHECK(CreateDirectoryW(content.c_str(), nullptr));
    Helper h(lab, L"--appid 480", lab.Fixture("{\"needsLegalAgreement\":true,\"uploadMs\":50,\"items\":[]}"));
    const json::Value p = h.Call("publish", "\"folder\":" + QPath(content) + ",\"title\":\"Hidden until accepted\"");
    CHECK(Ok(p) && p[L"needsLegalAgreement"].Bool() && p[L"itemId"].Str() == L"4000000000");
    CHECK(h.Close() == 0);
}

TEST(SteamHelperReportsAFailedUploadWithItsItem) {
    Lab lab;
    const std::wstring content = lab.dir + L"\\upload";
    CHECK(CreateDirectoryW(content.c_str(), nullptr));
    Helper h(lab, L"--appid 480", lab.Fixture("{\"fail\":{\"submit\":2},\"uploadMs\":50,\"items\":[]}"));
    const json::Value p = h.Call("publish", "\"folder\":" + QPath(content) + ",\"title\":\"Doomed\"");
    // The item was made before the upload failed: the answer names it, for trying again on it.
    CHECK(Error(p) == L"steam_error" && p[L"result"].Num() == 2 && p[L"itemId"].Str() == L"4000000000" && p[L"created"].Bool());
    CHECK(h.Close() == 0);

    Lab refused;
    const std::wstring other = refused.dir + L"\\upload";
    CHECK(CreateDirectoryW(other.c_str(), nullptr));
    Helper r(refused, L"--appid 480", refused.Fixture("{\"fail\":{\"create\":25},\"items\":[]}"));
    const json::Value c = r.Call("publish", "\"folder\":" + QPath(other) + ",\"title\":\"Never made\"");
    CHECK(Error(c) == L"steam_error" && c[L"result"].Num() == 25 && c[L"itemId"].IsNull());
    CHECK(r.Close() == 0);
}

TEST(SteamHelperExitsWhenItsInputCloses) {
    Lab lab;
    Helper h(lab, L"--appid 480", lab.Fixture("{\"items\":[]}"));
    CHECK(h.Call("status")[L"steam"].Str() == L"ok");
    CHECK(h.Close() == 0);
    CHECK(ReadText(lab.Log()).find("shutdown") != std::string::npos);
}
