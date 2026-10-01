// What install.exe and uninstall.exe actually do. Every step is reversible; uninstall runs
// the reverse order so the sign-in background never points at a file that is already gone.
#pragma once

#include <string>
#include <vector>

namespace engine {

struct Report {
    bool ok = false;
    std::wstring message;   // one line, for the console and the dialog
    std::vector<std::wstring> steps;
};

// The files install.exe copies, taken from beside install.exe.
extern const wchar_t *const kComponents[];
extern const size_t kComponentCount;

// Is the H.264 decoder present? Absent on Windows N/KN without the Media Feature Pack.
bool H264Available();

std::wstring DefaultInstallDir();

// Takes %ProgramData%\AnimeLogon (then opens install.log in it), copies the components to
// `targetDir`, which must lie inside Program Files, bakes the background, registers the
// uninstall entry, installs the service and only then applies the Windows lock-screen
// changes; a failure there undoes them. Ends with the Start-menu shortcut (needs COM).
Report Install(const std::wstring &targetDir);

// Stops and removes the service, restores the Windows lock screen, removes the shortcut and
// uninstall entry, and deletes the installed files (at the next restart if in use).
// `keepData` leaves imported videos and settings in place.
Report Uninstall(bool keepData);

}  // namespace engine
