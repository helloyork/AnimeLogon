// Where the themes, their wallpapers and their components are kept: the library.
//
// An install in Program Files keeps it in %ProgramData%\AnimeLogon, as before. An install on
// another drive goes into <drive>:\AnimeLogon, a directory the installer makes in the drive's root
// with an administrators-only DACL, and keeps the library beside its programs, in
// <drive>:\AnimeLogon\Library. The registry (HKLM\SOFTWARE\AnimeLogon, which only administrators
// can change) records that directory's path, the volume it is on and its file id, so that the
// library is found again after the drive's letter changes, and so that a directory standing in
// its place, or another volume given its letter, is not taken for it.
#pragma once

#include <windows.h>

#include <string>

namespace animelogon::library {

// The library's directory: %ProgramData%\AnimeLogon (paths::DataDir()) unless the registry
// pins one on another drive, then that directory at its volume's current drive letter (or at the
// recorded path when the volume has none just now). In a sandbox, always the sandbox's data dir.
std::wstring Dir();
// Whether the registry pins the library on a drive of its own.
bool Elsewhere();
// For a library on a drive of its own, what makes it one to trust beyond the directories in it:
// the drive at its letter is the volume installed to, the directory is the one pinned (same file
// id), <drive>:\AnimeLogon is held by administrators, and the drive's root lets nobody else delete
// what is in it or change who may. Always true for the library in %ProgramData%.
bool Verify(std::wstring *why);

// The directory an install on drive `letter` goes into, <letter>:\AnimeLogon, and its library.
std::wstring InstallDirOn(wchar_t letter);
std::wstring LibraryIn(const std::wstring &installDir);
// Whether `dir` is spelled exactly as InstallDirOn would spell it for some drive.
bool IsDriveInstallDir(const std::wstring &dir);

// Elevated callers only. Records `dir`, an existing directory spelled as LibraryIn spells it,
// as the library: its path, its volume and its file id.
DWORD Pin(const std::wstring &dir);
// Forgets the pin; the library is %ProgramData%\AnimeLogon again.
DWORD Unpin();

// Why a drive cannot hold an install, from facts gathered about it (the installer's Probe).
enum class Refusal {
    None,
    NotLocal,      // not a fixed local drive: a network drive, a CD, a drive letter made with subst
    Removable,     // on a USB, SD or similar bus: it can be unplugged, and another disk given its place
    Locked,        // its volume cannot be read now (BitLocker, locked)
    NoPermissions, // not NTFS or ReFS: FAT32 and exFAT keep no permissions
    LockedAtBoot,  // BitLocker without automatic unlocking: still locked when the logon screen starts
    OpenRoot,      // its root lets everyone delete what is in it or change who may
};
struct DriveFacts {
    wchar_t letter = 0;
    bool system = false;        // the drive Windows is on: installs go to Program Files there
    UINT type = 0;              // GetDriveType
    bool removableBus = false;
    bool readable = true;       // its volume information could be read
    std::wstring fileSystem;
    bool persistentAcls = false;
    bool lockedAtBoot = false;
    bool rootOpen = false;
};
Refusal Judge(const DriveFacts &drive);

// Tests only: makes Dir() return `dir` and Elsewhere() say whether it differs from DataDir.
// Empty goes back to the registry.
void UseForTests(const std::wstring &dir);

}  // namespace animelogon::library
