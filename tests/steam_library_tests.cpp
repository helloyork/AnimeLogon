// Finding an app's files and its Workshop downloads in Steam's libraries (app/src/steam_library.h),
// in folders the test lays out as Steam does.
#include "check.h"

#include <windows.h>

#include <string>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

#include "steam_library.h"

using namespace app::steam_library;

namespace {

// A fresh directory under %TEMP%, in its long form so paths compare equal.
std::wstring Scratch() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-steamlib-" + animelogon::RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool Write(const std::wstring &path, const std::string &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    return CloseHandle(h) && ok;
}

std::string Manifest(const char *installdir) {
    return std::string("\"AppState\"\n{\n\t\"appid\"\t\t\"431960\"\n\t\"name\"\t\t\"Wallpaper Engine\"\n\t\"installdir\"\t\t\"") +
           installdir + "\"\n\t\"UserConfig\"\n\t{\n\t\t\"language\"\t\t\"english\"\n\t}\n}\n";
}

}  // namespace

TEST(SteamLibraryReadsAnAppsInstallFolderFromItsManifest) {
    CHECK(ManifestInstallDir(Manifest("wallpaper_engine")) == L"wallpaper_engine");
    CHECK(ManifestInstallDir(Manifest("Some Game: Deluxe")).empty());  // not a plain folder name
    CHECK(ManifestInstallDir(Manifest("..")).empty());
    CHECK(ManifestInstallDir(Manifest("..\\\\..\\\\Windows")).empty());
    CHECK(ManifestInstallDir(Manifest("")).empty());
    // Only the outermost block's key counts.
    CHECK(ManifestInstallDir("\"AppState\" { \"Inner\" { \"installdir\" \"x\" } }").empty());
    CHECK(ManifestInstallDir("\"AppState\" { \"InstallDir\" \"y\" }") == L"y");
    CHECK(ManifestInstallDir("").empty());
}

TEST(SteamLibraryFindsAnAppsFilesAndWorkshopDownloads) {
    const std::wstring root = Scratch();
    const std::wstring a = root + L"\\Steam", b = root + L"\\Library B";
    CHECK(WorkshopContentPath(a + L"\\", 480) == a + L"\\steamapps\\workshop\\content\\480");
    CHECK(WorkshopContentPath(L"E:/Games/", 480) == L"E:\\Games\\steamapps\\workshop\\content\\480");

    // Workshop downloads for app 480 in both libraries, for another app in one only.
    for (const std::wstring &d : {a + L"\\steamapps\\workshop\\content\\480\\111", b + L"\\steamapps\\workshop\\content\\480",
                                  b + L"\\steamapps\\workshop\\content\\7", b + L"\\steamapps\\common\\Space War"})
        animelogon::paths::CreateDirectories(d);
    // Library A has a manifest whose folder is gone (the app moved), library B the real one.
    CHECK(Write(a + L"\\steamapps\\appmanifest_480.acf", Manifest("Space War")));
    CHECK(Write(b + L"\\steamapps\\appmanifest_480.acf", Manifest("Space War")));

    const std::vector<std::wstring> content = WorkshopContentIn({a, b, a + L"\\", root + L"\\nowhere"}, 480);
    CHECK(content.size() == 2);
    CHECK(content.size() == 2 && content[0] == a + L"\\steamapps\\workshop\\content\\480" &&
          content[1] == b + L"\\steamapps\\workshop\\content\\480");
    CHECK(WorkshopContentIn({a}, 7).empty());
    CHECK(WorkshopContentIn({}, 480).empty());

    CHECK(AppInstallDirIn({a, b}, 480) == b + L"\\steamapps\\common\\Space War");
    CHECK(AppInstallDirIn({a}, 480).empty());
    CHECK(AppInstallDirIn({a, b}, 7).empty());  // no manifest

    animelogon::secure::RemoveTree(root);
}

// What this computer has; the test only checks that asking is safe whatever is installed.
TEST(SteamLibraryAsksThisComputerWithoutFailing) {
    const std::vector<std::wstring> libraries = Libraries();
    if (!libraries.empty()) CHECK(libraries[0] == SteamDir());
    for (const std::wstring &l : libraries) CHECK(!l.empty() && l.back() != L'\\');
    for (const std::wstring &c : WorkshopContent(431960)) CHECK(c.find(L"\\steamapps\\workshop\\content\\431960") != std::wstring::npos);
    (void)AppInstallDir(431960);
}
