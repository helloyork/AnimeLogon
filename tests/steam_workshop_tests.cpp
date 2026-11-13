// The library's Steam Workshop items (app/src/steam_workshop.h) and the settings app's reading of
// the Steam helper's output (app/src/steam_client.h Lines): workshop.ini, an item's folder, what
// state an item is shown in, and which of the installed wallpapers and components an update may
// take away with the theme it replaces, against store.cpp's own finding of them.
#include "check.h"

#include <windows.h>

#include <string>
#include <vector>

#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "commit.h"
#include "json.h"
#include "scratch.h"
#include "steam_client.h"
#include "steam_workshop.h"
#include "store.h"

using namespace animelogon;
namespace sw = app::steam_workshop;

namespace {

std::wstring Scratch() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-workshop-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool Write(const std::wstring &path, const void *bytes, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes, (DWORD)size, &wrote, nullptr) && wrote == size;
    CloseHandle(h);
    return ok;
}
bool Write(const std::wstring &path, const std::string &text) { return Write(path, text.data(), text.size()); }
bool Write(const std::wstring &path, const std::vector<uint8_t> &bytes) { return Write(path, bytes.data(), bytes.size()); }

void SetTime(const std::wstring &path, long long unix) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const unsigned long long ticks = (unsigned long long)unix * 10000000ull + 116444736000000000ull;
    FILETIME ft{(DWORD)ticks, (DWORD)(ticks >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

std::vector<uint8_t> Bmp(int w, int h, uint8_t seed) {
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (size_t i = 0; i < bgra.size(); ++i) bgra[i] = (i % 4 == 3) ? 255 : (uint8_t)(i * 5 + seed);
    return bitmap::Build(w, h, bgra.data(), bgra.size());
}

// A package staged as the importer stages one: an image wallpaper and one component.
std::wstring StagePackage(const std::wstring &scratch, uint8_t picture, const std::string &component) {
    const std::wstring dir = scratch + L"\\AnimeLogon\\import\\" + RandomHex(6);
    paths::CreateDirectories(dir + L"\\wallpaper");
    paths::CreateDirectories(dir + L"\\components");
    theme::Theme t;
    t.name = L"Seaside";
    t.wallpaper = L"wallpaper.png";
    t.components.push_back({L"clock", L"clock", true, {}});
    t.components.push_back({L"badge", L"components/badge.xml", true, {}});
    CHECK(Write(dir + L"\\theme.xml", theme::Normalize(t)));
    WallpaperInfo info;
    info.kind = WallpaperKind::Image;
    info.name = L"Seaside";
    info.width = 8;
    info.height = 6;
    CHECK(Write(dir + L"\\wallpaper\\wallpaper.ini", ToUtf8(SerializeWallpaperInfo(info))));
    CHECK(Write(dir + L"\\wallpaper\\image.bmp", Bmp(8, 6, picture)));
    CHECK(Write(dir + L"\\components\\badge.xml", component));
    return dir;
}

// The clock under another name: a component of its own.
std::string Component(const std::string &name) {
    std::string text = skin::DefaultText();
    const std::string from = u8"name=\"时钟\"";
    text.replace(text.find(from), from.size(), "name=\"" + name + "\"");
    return text;
}

}  // namespace

TEST(SteamItemIdsAreWholeNumbersAsTheHelperWritesThem) {
    using app::steam::IsItemId;
    CHECK(IsItemId(L"3100000001") && IsItemId(L"1") && IsItemId(L"18446744073709551615"));
    CHECK(!IsItemId(L"") && !IsItemId(L"0") && !IsItemId(L"012") && !IsItemId(L"18446744073709551616"));
    CHECK(!IsItemId(L"12a") && !IsItemId(L"-5") && !IsItemId(L"..\\x") && !IsItemId(L"123456789012345678901"));
}

TEST(SteamHelperOutputIsCutIntoLinesWhereverItIsSplit) {
    app::steam::Lines lines(16);
    std::string line;
    // A line in pieces, one with a '\r' before its end, and two at once.
    lines.Feed("{\"id\"", 5);
    CHECK(!lines.Next(&line));
    lines.Feed(":1}\r", 4);
    CHECK(!lines.Next(&line));
    lines.Feed("\n{\"a\":2}\n{\"b\":3}\n{", 19);
    CHECK(lines.Next(&line) && line == "{\"id\":1}");
    CHECK(lines.Next(&line) && line == "{\"a\":2}");
    CHECK(lines.Next(&line) && line == "{\"b\":3}");
    CHECK(!lines.Next(&line));
    // Longer than the limit: dropped whole, and the lines after it still come.
    const std::string longer = "}" + std::string(40, 'x') + "\nok\n";
    lines.Feed(longer.data(), longer.size());
    CHECK(lines.Next(&line) && line == "ok" && lines.dropped() == 1);
    CHECK(!lines.Next(&line));
    lines.Feed("\n", 1);
    CHECK(lines.Next(&line) && line.empty());
}

TEST(WorkshopIniKeepsWhatWasInstalledFromEachItem) {
    sw::Records r;
    r[L"3100000001"] = {L"0123456789abcdef", L"夜樱\nNight Sakura", 1762000000, 1762000123};
    r[L"42"] = {L"fedcba9876543210", L"", 0, 0};
    r[L"0"] = {L"0123456789abcdef", L"not an item", 1, 1};      // not an item id: not written
    r[L"77"] = {L"..\\..\\x", L"not a theme", 1, 1};             // not a theme id: not written
    const std::wstring text = sw::FormatRecords(r);
    const sw::Records back = sw::ParseRecords(text);
    CHECK(back.size() == 2);
    CHECK(back.at(L"3100000001").theme == L"0123456789abcdef" && back.at(L"3100000001").title == L"夜樱 Night Sakura");
    CHECK(back.at(L"3100000001").updated == 1762000000 && back.at(L"3100000001").file == 1762000123);
    CHECK(back.at(L"42").theme == L"fedcba9876543210" && back.at(L"42").updated == 0);

    // Hand-made: a byte order mark, comments, unknown sections and keys, a section without a theme.
    const sw::Records odd = sw::ParseRecords(
        L"\xFEFF; note\n[installed 5]\ntheme = 00000000000000aa\nupdated = 9\ncolour = red\n\n[published 6]\ntheme = 00000000000000bb\n"
        L"[installed 7]\nupdated = 3\n[installed 8\ntheme = 00000000000000cc\n[installed 9]\r\ntheme=00000000000000dd\r\nfile = -4\r\n");
    CHECK(odd.size() == 2 && odd.at(L"5").theme == L"00000000000000aa" && odd.at(L"5").updated == 9);
    CHECK(odd.at(L"9").theme == L"00000000000000dd" && odd.at(L"9").file == 0);

    // On disk, replaced whole; a missing file reads as nothing installed.
    const std::wstring dir = Scratch();
    const std::wstring path = dir + L"\\sub\\workshop.ini";
    CHECK(sw::LoadRecords(path).empty());
    CHECK(sw::SaveRecords(path, r));
    CHECK(sw::LoadRecords(path).size() == 2);
    r.erase(L"42");
    CHECK(sw::SaveRecords(path, r) && sw::LoadRecords(path).size() == 1);
    CHECK(GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(sw::RecordsPath() == paths::UserDataDir() + L"\\workshop.ini");
    secure::RemoveTree(dir);
}

TEST(WorkshopItemFolderMustHoldExactlyOneThemePackage) {
    const std::wstring dir = Scratch();
    CHECK(sw::Inspect(dir + L"\\missing").contents == sw::Contents::Missing);
    CHECK(sw::Inspect(L"").contents == sw::Contents::Missing);
    const std::wstring item = dir + L"\\3100000001";
    CreateDirectoryW(item.c_str(), nullptr);
    CHECK(sw::Inspect(item).contents == sw::Contents::None);
    // Other files are passed over; the extension is matched in any case.
    CHECK(Write(item + L"\\preview.jpg", std::string("jpeg")));
    CHECK(Write(item + L"\\Night Sakura.ALTHEME", std::string("0123456789")));
    SetTime(item + L"\\Night Sakura.ALTHEME", 1762000123);
    sw::Folder f = sw::Inspect(item);
    CHECK(f.contents == sw::Contents::Ok && f.package == item + L"\\Night Sakura.ALTHEME" && f.size == 10 && f.time == 1762000123);
    CHECK(Write(item + L"\\second.altheme", std::string("x")));
    f = sw::Inspect(item);
    CHECK(f.contents == sw::Contents::Several && f.package.empty());
    // A folder by the name is not a package.
    const std::wstring other = dir + L"\\3100000002";
    CreateDirectoryW(other.c_str(), nullptr);
    CreateDirectoryW((other + L"\\theme.altheme").c_str(), nullptr);
    CHECK(sw::Inspect(other).contents == sw::Contents::NotPlain);
    CHECK(std::string(sw::ProblemKey(sw::Contents::Several)) == "steam.problem.several");
    CHECK(std::string(sw::ProblemKey(sw::Contents::Ok)).empty());

    // Without Steam, the items are the folders named by item ids, each once.
    const std::wstring second = dir + L"\\library2";
    CreateDirectoryW(second.c_str(), nullptr);
    CreateDirectoryW((second + L"\\3100000001").c_str(), nullptr);
    CreateDirectoryW((second + L"\\99").c_str(), nullptr);
    CreateDirectoryW((second + L"\\notes").c_str(), nullptr);
    const std::vector<sw::Item> found = sw::FromFolders({dir, second, dir + L"\\none"});
    CHECK(found.size() == 3 && found[0].id == L"99" && found[1].id == L"3100000001" && found[2].id == L"3100000002");
    CHECK(found[1].folder == item && found[1].subscribed && found[1].installed && found[1].timeUpdated == 0);
    secure::RemoveTree(dir);
}

TEST(WorkshopItemsComeFromTheHelpersSubscribedAnswer) {
    json::Value answer;
    CHECK(json::Parse(LR"({"id":5,"ok":true,"details":true,"items":[
        {"id":"3100000001","state":{"subscribed":true,"installed":true,"needsUpdate":false,"downloading":false,"downloadPending":false},
         "folder":"D:\\Steam\\3100000001","sizeOnDisk":182000000,"timeStamp":1762000000,"title":"Night","timeUpdated":1762000000,
         "preview":"https://images.steamusercontent.com/ugc/1/","descriptors":["violenceGore"]},
        {"id":"3100000002","state":{"subscribed":true,"installed":false,"needsUpdate":false,"downloading":false,"downloadPending":true}},
        {"id":"not a number","state":{}}]})", &answer));
    const std::vector<sw::Item> items = sw::FromSubscribed(answer);
    CHECK(items.size() == 2);
    CHECK(items[0].id == L"3100000001" && items[0].installed && !items[0].downloading && items[0].folder == L"D:\\Steam\\3100000001");
    CHECK(items[0].title == L"Night" && items[0].timeUpdated == 1762000000 && items[0].sizeOnDisk == 182000000);
    CHECK(items[0].descriptors.size() == 1 && items[0].descriptors[0] == L"violenceGore");
    CHECK(!items[1].installed && items[1].downloading && items[1].title.empty() && items[1].timeUpdated == 0);
}

TEST(WorkshopItemStateFollowsSteamTheFolderAndWorkshopIni) {
    using S = sw::State;
    sw::Item item;
    item.id = L"3100000001";
    item.subscribed = true;
    item.installed = true;
    item.timeUpdated = 200;
    sw::Folder ok;
    ok.contents = sw::Contents::Ok;
    ok.time = 50;
    sw::Folder several;
    several.contents = sw::Contents::Several;
    const sw::Folder missing;
    sw::Record record{L"0123456789abcdef", L"Night", 200, 50};

    // Not installed.
    CHECK(sw::StateOf(item, ok, nullptr, false) == S::NotInstalled);
    CHECK(sw::StateOf(item, several, nullptr, false) == S::Unreadable);
    CHECK(sw::StateOf(item, missing, nullptr, false) == S::NotDownloaded);
    sw::Item notDownloaded = item;
    notDownloaded.installed = false;
    CHECK(sw::StateOf(notDownloaded, ok, nullptr, false) == S::NotDownloaded);
    sw::Item downloading = notDownloaded;
    downloading.downloading = true;
    CHECK(sw::StateOf(downloading, missing, nullptr, false) == S::Downloading);
    // A record whose theme was deleted since counts for nothing.
    CHECK(sw::StateOf(item, ok, &record, false) == S::NotInstalled);

    // Installed.
    CHECK(sw::StateOf(item, ok, &record, true) == S::Installed);
    CHECK(sw::Installable(S::NotInstalled) && sw::Installable(S::Update) && !sw::Installable(S::Installed));
    // Steam has a newer version: downloading it first, then ready to install over the old one.
    sw::Item newer = item;
    newer.timeUpdated = 300;
    newer.needsUpdate = true;
    CHECK(sw::StateOf(newer, ok, &record, true) == S::Downloading);
    newer.needsUpdate = false;
    CHECK(sw::StateOf(newer, ok, &record, true) == S::Update);
    CHECK(sw::StateOf(newer, several, &record, true) == S::Unreadable);
    // Without Steam's times, by the package having been written since.
    sw::Item offline = item;
    offline.timeUpdated = 0;
    CHECK(sw::StateOf(offline, ok, &record, true) == S::Installed);
    sw::Folder rewritten = ok;
    rewritten.time = 60;
    CHECK(sw::StateOf(offline, rewritten, &record, true) == S::Update);
    // Steam's times decide when both are known, whatever the file's time.
    CHECK(sw::StateOf(item, rewritten, &record, true) == S::Installed);
    // No longer subscribed to: the theme stays, as unsubscribed.
    sw::Item gone = item;
    gone.subscribed = false;
    CHECK(sw::StateOf(gone, missing, &record, true) == S::Unsubscribed);
    CHECK(std::wstring(sw::NameOf(S::Unsubscribed)) == L"unsubscribed" && std::wstring(sw::NameOf(S::NotInstalled)) == L"notInstalled");
}

TEST(AnUpdateTakesAwayOnlyWhatTheStagedPackagesDoNotUseAgain) {
    const std::wstring scratch = Scratch();
    scratch::PlainTarget t(scratch + L"\\data");
    // The installed version: its picture and its component.
    store::PackageResult old;
    CHECK(store::CommitPackage(t, StagePackage(scratch, 1, Component("Badge")), &old) == commit::kOk);
    WallpaperInfo installed;
    std::vector<uint8_t> ini;
    CHECK(secure::ReadFileBytes(store::In(t, WallpaperInfoPath(old.wallpaperId)), &ini, 65536));
    CHECK(ParseWallpaperInfo(FromUtf8(std::string(ini.begin(), ini.end())), &installed));
    const std::string installedComponent = sw::NormalizedComponent(store::In(t, ComponentFilePath(old.components[L"components/badge.xml"])));
    CHECK(!installed.sha256.empty() && !installedComponent.empty());

    // Its update with the same picture and another component: the picture is used again.
    const std::wstring same = StagePackage(scratch, 1, Component("Badge 2"));
    CHECK(sw::StagedWallpaperHash(same + L"\\wallpaper") == installed.sha256);
    sw::Reuse r = sw::StagedReuse({same});
    CHECK(!r.MayRemoveWallpaper(installed.sha256) && r.MayRemoveComponent(installedComponent));
    // And store.cpp agrees.
    store::PackageResult update;
    CHECK(store::CommitPackage(t, same, &update) == commit::kOk && update.wallpaperReused && update.componentsReused.empty());

    // Another picture and the same component: the component is used again, the picture may go.
    const std::wstring other = StagePackage(scratch, 9, Component("Badge"));
    r = sw::StagedReuse({other});
    CHECK(r.MayRemoveWallpaper(installed.sha256) && !r.MayRemoveComponent(installedComponent));
    store::PackageResult third;
    CHECK(store::CommitPackage(t, other, &third) == commit::kOk && !third.wallpaperReused && third.componentsReused.size() == 1);

    // Several staged together: what any of them uses again stays.
    r = sw::StagedReuse({same, other});
    CHECK(!r.MayRemoveWallpaper(installed.sha256) && !r.MayRemoveComponent(installedComponent));
    // One that cannot be looked at: nothing is taken away.
    const std::wstring scene = StagePackage(scratch, 3, Component("X"));
    DeleteFileW((scene + L"\\wallpaper\\image.bmp").c_str());
    r = sw::StagedReuse({scene});
    CHECK(!r.wallpapersKnown && !r.MayRemoveWallpaper(installed.sha256));
    CHECK(sw::StagedReuse({}).MayRemoveWallpaper(installed.sha256) && !sw::Reuse().MayRemoveWallpaper(L""));
    secure::RemoveTree(scratch);
}
