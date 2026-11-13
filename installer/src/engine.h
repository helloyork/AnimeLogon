// What install.exe and uninstall.exe actually do. Every step is reversible; uninstall runs
// the reverse order so the sign-in background never points at a file that is already gone.
#pragma once

#include <functional>
#include <string>
#include <vector>

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
};

// The program files, installed from inside install.exe (payload.h), or from beside it where a
// build carries none.
extern const wchar_t *const kComponents[];
extern const size_t kComponentCount;

// Is the H.264 decoder present? Absent on Windows N/KN without the Media Feature Pack.
bool H264Available();
// Whether the Edge WebView2 runtime is installed, as Microsoft says to find out: its client key.
bool WebView2Installed();

std::wstring ProgramFilesDir();
std::wstring DefaultInstallDir();
// The SYSTEM service runs from the install directory, so it must lie inside Program Files.
bool InstallDirAllowed(const std::wstring &dir);
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
    Progress progress;
};

// Takes %ProgramData%\AnimeLogon and its wallpapers\, components\ and themes\ (then opens
// install.log in it), deletes the videos\ and skins\ stores from before themes, closes a settings
// window running from `targetDir`, writes the program files to `targetDir`, which must lie inside
// Program Files, and deletes config.exe there (the settings app of earlier versions), bakes the
// background, writes default settings (the default theme) unless settings.ini already names a
// theme, registers the uninstall entry, installs the service and only then applies the Windows
// lock-screen changes; a failure there undoes them. Ends with the shortcuts (needs COM), and says
// when the settings app will not find the WebView2 runtime.
Report Install(const std::wstring &targetDir, const InstallOptions &options = {});

// Closes a settings window running from the install directory, removes the shortcuts and the
// uninstall entry, stops and removes the service, restores the Windows lock screen, and deletes the
// installed files (at the next restart if in use). `keepData` leaves the themes, their wallpapers
// and components, and settings.ini in place; otherwise all of %ProgramData%\AnimeLogon goes.
Report Uninstall(bool keepData, const Progress &progress = {});

}  // namespace engine
