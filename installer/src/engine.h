// What install.exe and uninstall.exe actually do. Every step is reversible; uninstall runs
// the reverse order so the sign-in background never points at a file that is already gone.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "animelogon/library.h"

namespace engine {

// How far the work is (0 to 1) and the step it is on, as it goes; called on the working thread.
using Progress = std::function<void(double fraction, const std::wstring &step)>;

struct Report {
    bool ok = false;
    std::wstring message;   // a sentence or two, for the console and the dialog
    std::vector<std::wstring> steps;
    std::vector<std::wstring> problems;  // uninstall: what could not be undone
    bool webView2Missing = false;        // install: the settings app will not open until it is installed
    bool restartPending = false;         // uninstall: files in use go at the next restart
    std::wstring switchedOff;            // install: another program drawing the logon screen it switched off
    std::vector<std::wstring> othersOff; // uninstall: such programs left off, to be switched on in their own settings
    std::wstring keptAt;                 // uninstall: where the themes that stay are (the library)
};

// The files installed are payload.h's: from inside install.exe, the programs for the processor
// Windows runs on, or from beside it where a build carries none.

// Is the H.264 decoder present? Absent on Windows N/KN without the Media Feature Pack.
bool H264Available();
// Whether the Edge WebView2 runtime is installed, as Microsoft says to find out: its client key.
bool WebView2Installed();

std::wstring ProgramFilesDir();
std::wstring DefaultInstallDir();
// The SYSTEM service runs from the install directory, so it must lie inside Program Files, or be
// <drive>:\AnimeLogon on another local drive that library::Judge does not refuse (drives.h).
bool InstallDirAllowed(const std::wstring &dir);
// Why `dir` cannot take an install, in a sentence; empty when it can.
std::wstring DirRefusal(const std::wstring &dir);
// A sentence for why a drive cannot hold an install.
std::wstring RefusalText(animelogon::library::Refusal why);
// The version in the uninstall entry; empty when AnimeLogon is not installed.
std::wstring InstalledVersion();
// Free space on the volume `dir` would be on (its nearest existing ancestor's).
unsigned long long FreeBytes(const std::wstring &dir);
unsigned long long RequiredBytes();

// What stops an install before it starts: each with what to do about it. Empty when it can go.
struct Blocker {
    std::wstring title, detail;
};
std::vector<Blocker> CheckBeforeInstall(const std::wstring &targetDir);

struct InstallOptions {
    bool desktopShortcut = false;
    std::wstring language;  // written as the machine's language; empty leaves it as it is
    // Leaves switching on to the person, in the settings app (machine::SetupPending): Windows is
    // not touched and no other program is switched off. A new install is marked so the settings
    // app opens on its setup wizard; an update leaves AnimeLogon on or off as it was.
    bool setupInApp = false;
    Progress progress;
};

// Changes nothing where there are no programs for the processor Windows runs on
// (payload::InstallMachine), and installs that processor's programs otherwise. Stops the service of
// an install being updated, and starts it again if the update fails before its own is installed.
// On a drive other than Windows' own, makes <drive>:\AnimeLogon and its Library administrators-only
// and pins the library there (library.h), refusing a directory of that name somebody else made; in
// Program Files, unpins it. Takes %ProgramData%\AnimeLogon and the library's wallpapers\,
// components\ and themes\ (then opens install.log in ProgramData), deletes the videos\ and skins\
// stores from before themes, closes AnimeLogon's own windows on this desktop (the settings app, the
// icon in the notification area), writes the program files to `targetDir` (InstallDirAllowed; an
// update does not move to or from another drive) -- a file still in use elsewhere
// is moved aside and deleted at the next restart -- and deletes config.exe there (the settings app
// of earlier versions), bakes the background, writes default settings (the default theme) unless
// settings.ini already names a theme, registers the uninstall entry and .altheme files (assoc.h),
// installs the service, switches off other programs that draw the logon screen (others.h) and only
// then applies the Windows lock-screen changes; a failure there undoes them (with `setupInApp`,
// neither of those last two: see InstallOptions). Ends with the shortcuts, the Run entry that shows
// the icon at sign-in and the icon itself for whoever is signed in here (needs COM), and says
// when the settings app will not find the WebView2 runtime.
Report Install(const std::wstring &targetDir, const InstallOptions &options = {});

// Closes AnimeLogon's windows on this desktop, removes the shortcuts, the Run entry, the
// uninstall entry and what registered .altheme files (assoc.h), stops and removes the service,
// restores the Windows lock screen, and deletes the installed files (at the next restart if in
// use). `keepData` leaves the themes, their wallpapers and components, and settings.ini in place
// (a library on another drive keeps <drive>:\AnimeLogon too, for a later install there to find);
// otherwise all of %ProgramData%\AnimeLogon goes, and the library wherever it is.
Report Uninstall(bool keepData, const Progress &progress = {});

}  // namespace engine
