// Where the library is (library.h): the drives an install may go on, the directory an install on
// another drive goes into, the stores following the library, and the root check that keeps such a
// directory from being taken away and replaced.
#include "check.h"

#include <windows.h>
#include <sddl.h>

#include <string>

#include "animelogon/components.h"
#include "animelogon/library.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "scratch.h"
#include "store.h"

using namespace animelogon;

namespace {

library::DriveFacts GoodDrive() {
    library::DriveFacts d;
    d.letter = L'D';
    d.type = DRIVE_FIXED;
    d.fileSystem = L"NTFS";
    d.persistentAcls = true;
    return d;
}

// A fresh directory under %TEMP% with the given security descriptor.
std::wstring DirWith(const wchar_t *sddl) {
    wchar_t temp[MAX_PATH + 1];
    GetTempPathW(ARRAYSIZE(temp), temp);
    std::wstring dir = temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-root-" + RandomHex(6);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return L"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const bool made = CreateDirectoryW(dir.c_str(), &sa) != FALSE;
    LocalFree(sd);
    return made ? dir : L"";
}

}  // namespace

TEST(LibraryRefusesDrivesTheLogonScreenCannotRelyOn) {
    CHECK(library::Judge(GoodDrive()) == library::Refusal::None);
    library::DriveFacts d = GoodDrive();
    d.fileSystem = L"ReFS";
    CHECK(library::Judge(d) == library::Refusal::None);
    d = GoodDrive();
    d.type = DRIVE_REMOTE;
    CHECK(library::Judge(d) == library::Refusal::NotLocal);
    d.type = DRIVE_REMOVABLE;
    CHECK(library::Judge(d) == library::Refusal::NotLocal);
    d = GoodDrive();
    d.removableBus = true;  // a USB disk that calls itself fixed
    CHECK(library::Judge(d) == library::Refusal::Removable);
    d = GoodDrive();
    d.readable = false;
    CHECK(library::Judge(d) == library::Refusal::Locked);
    d = GoodDrive();
    d.fileSystem = L"exFAT";
    d.persistentAcls = false;
    CHECK(library::Judge(d) == library::Refusal::NoPermissions);
    d = GoodDrive();
    d.persistentAcls = false;
    CHECK(library::Judge(d) == library::Refusal::NoPermissions);
    d = GoodDrive();
    d.lockedAtBoot = true;
    CHECK(library::Judge(d) == library::Refusal::LockedAtBoot);
    d = GoodDrive();
    d.rootOpen = true;
    CHECK(library::Judge(d) == library::Refusal::OpenRoot);
    // Windows' own drive installs in Program Files, whatever else is said of it.
    d.system = true;
    CHECK(library::Judge(d) == library::Refusal::None);
}

TEST(LibraryInstallDirIsTheDrivesOwnAnimeLogonFolder) {
    CHECK(library::InstallDirOn(L'd') == L"D:\\AnimeLogon");
    CHECK(library::LibraryIn(L"D:\\AnimeLogon") == L"D:\\AnimeLogon\\Library");
    CHECK(library::IsDriveInstallDir(L"D:\\AnimeLogon"));
    CHECK(library::IsDriveInstallDir(L"Z:\\AnimeLogon"));
    CHECK(!library::IsDriveInstallDir(L"d:\\AnimeLogon"));
    CHECK(!library::IsDriveInstallDir(L"D:\\animelogon"));
    CHECK(!library::IsDriveInstallDir(L"D:\\Program\\AnimeLogon"));
    CHECK(!library::IsDriveInstallDir(L"D:\\AnimeLogon\\Library"));
    CHECK(!library::IsDriveInstallDir(L"D:\\AnimeLogon\\"));
    CHECK(!library::IsDriveInstallDir(L"\\\\server\\AnimeLogon"));
    // Nothing pinned without an administrator's registry values; pinning refuses another shape.
    CHECK(library::Pin(L"D:\\Program\\AnimeLogon\\Library") == ERROR_INVALID_PARAMETER);
    CHECK(library::Pin(L"D:\\AnimeLogon") == ERROR_INVALID_PARAMETER);
}

TEST(LibraryStoresFollowTheLibrary) {
    // Unpinned (the tests never write HKLM): the stores are ProgramData's, as before.
    CHECK(library::Dir() == paths::DataDir());
    CHECK(!library::Elsewhere());
    CHECK(WallpapersDir() == paths::DataDir() + L"\\wallpapers");
    library::UseForTests(L"D:\\AnimeLogon\\Library");
    CHECK(library::Elsewhere());
    CHECK(WallpapersDir() == L"D:\\AnimeLogon\\Library\\wallpapers");
    CHECK(ThemesDir() == L"D:\\AnimeLogon\\Library\\themes");
    CHECK(ComponentsDir() == L"D:\\AnimeLogon\\Library\\components");
    // The elevated helper's paths move under its target as before, from the library now.
    const scratch::PlainTarget target(L"C:\\scratch\\data");
    CHECK(store::In(target, WallpaperDir(L"0123456789abcdef")) == L"C:\\scratch\\data\\wallpapers\\0123456789abcdef");
    CHECK(store::In(target, paths::DataDir() + L"\\settings.ini") == paths::DataDir() + L"\\settings.ini");
    // What the logon screen reads from the library is checked against the library's directories.
    std::wstring why;
    CHECK(!secure::IsTrustedDirectory(L"D:\\AnimeLogon\\Library\\wallpapers\\0123456789abcdef", &why));
    library::UseForTests(L"");
    CHECK(library::Dir() == paths::DataDir());
}

TEST(LibraryRootCheckLooksForDeletingAndTakingOver) {
    // Administrators and SYSTEM in full, Users reading, Authenticated Users adding: a root as
    // Windows formats a drive, which keeps a directory in it.
    const std::wstring held = DirWith(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)(A;;0x1301bf;;;AU)");
    // Everyone may delete children: a directory in it could be renamed away and replaced.
    const std::wstring open = DirWith(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)(A;;0x1200e9;;;WD)");
    // Users may change who may.
    const std::wstring dac = DirWith(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)(A;;0x1600a9;;;BU)");  // read + WRITE_DAC
    CHECK(!held.empty() && !open.empty() && !dac.empty());
    std::wstring who;
    CHECK(!secure::RootIsOpen(held, &who));
    CHECK(secure::RootIsOpen(open, &who) && who.find(L"S-1-1-0") != std::wstring::npos);
    CHECK(secure::RootIsOpen(dac, &who));
    CHECK(secure::RootIsOpen(held + L"\\missing", &who));
    for (const std::wstring &d : {held, open, dac})
        if (!d.empty()) RemoveDirectoryW(d.c_str());
}
