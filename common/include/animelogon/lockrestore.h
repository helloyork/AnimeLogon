// Every account's own lock screen picture, put back when AnimeLogon is removed.
//
// AnimeLogon sets each signed-in account's lock screen picture (signin.h) and records what the
// account had in its own HKCU\Software\AnimeLogon\LockScreen (app/src/lockscreen.h). Only the
// account itself can set it back, from its own session: the service does so at once for every
// account signed in when it is asked (machine::AskServiceToRestore). Every other account that still
// has a record gets a one-time step at its next sign-in: a small PowerShell script in its own
// %LOCALAPPDATA%\AnimeLogon, run from its RunOnce key, which sets the picture back, puts Windows
// Spotlight's switch back and deletes the record, the copy and itself. If AnimeLogon is installed
// again by then, the script leaves the record for that install and only deletes itself.
#pragma once

#include <string>

namespace animelogon::lockrestore {

// The script, ASCII.
const char *Script();
// The RunOnce command that runs the script at `scriptPath`.
std::wstring CommandFor(const std::wstring &scriptPath);

// For the uninstaller (administrators only): gives each account with a record its one-time step.
// Returns how many accounts got it; `unreachable` counts accounts with a profile whose settings
// could not be read or written.
int ScheduleForAccounts(int *unreachable);

}  // namespace animelogon::lockrestore
