// Machine-wide changes AnimeLogon makes, and how each is undone. Administrators only.
#pragma once

#include <windows.h>

#include <string>

namespace animelogon::machine {

// --- Registry values Windows reads ----------------------------------------------------
// Each is recorded under HKLM\SOFTWARE\AnimeLogon\Restore the first time it is changed,
// and put back exactly as it was by RestoreWindowsLockScreen.

// Turns Windows' own lock screen off, so the credential screen (and the video over it)
// comes up directly. The sign-in background is each person's own lock screen picture, which
// their agent sets (signin.h); a PersonalizationCSP picture AnimeLogon 1.0 set is put back.
DWORD ApplyWindowsLockScreen();
DWORD RestoreWindowsLockScreen();
bool WindowsLockScreenApplied();

// Group policy that makes Windows ignore the lock screen image and paint a flat colour.
bool LogonBackgroundDisabledByPolicy();

// Each account's "Show lock screen background picture on the sign-in screen". Windows keeps
// it where only SYSTEM may write, so these run in the service. Show turns it on wherever it
// is off, recording the original; Restore puts the originals back.
DWORD ShowSignInBackground();
DWORD RestoreSignInBackground();
bool SignInBackgroundRestorePending();
// The service's control code for putting the sign-in background back -- RestoreSignInBackground,
// and the lock screen picture of the person at the console, which their own session puts back --
// and the start argument that does it once in a service that is not running.
constexpr DWORD kServiceControlRestore = 128;
constexpr const wchar_t *kServiceRestoreArg = L"--restore";
// Has the service put the sign-in background back; fails only while a recorded setting is left.
DWORD AskServiceToRestore();

// --- The service -------------------------------------------------------------------
DWORD InstallService(const std::wstring &launcherPath);
DWORD RemoveService();
DWORD StartLauncherService();
DWORD StopLauncherService(DWORD waitMs = 15000);
// SERVICE_RUNNING, SERVICE_STOPPED... or 0 if the service is not installed.
DWORD ServiceState();
DWORD SetServiceAutoStart(bool automatic);
bool ServiceAutoStart();

// --- The switch in the settings app ----------------------------------------------------
// On: Windows' lock screen off, the service automatic and running,
// the paused marker gone, setup finished (below); if any step fails, it is all undone
// again. Off: all of it undone, files and settings kept.
DWORD TurnOn(const std::wstring &launcherPath);
DWORD TurnOff();
bool IsOn();

// Installed but never switched on: an install that left switching on to the person (Steam's
// install script runs one), so the settings app opens on its setup wizard until they do.
// Kept under HKLM\SOFTWARE\AnimeLogon, so uninstalling forgets it; TurnOn clears it.
bool SetupPending();
DWORD MarkSetupPending();

bool Paused();                    // Ctrl+Alt+F11 was pressed on the logon screen
bool WritePausedMarker(const wchar_t *reason);
bool ClearPausedMarker();

// --- Installation record -------------------------------------------------------------
std::wstring InstallDir();        // from HKLM\SOFTWARE\AnimeLogon, empty if not installed
DWORD WriteInstallRecord(const std::wstring &dir);
// Removes HKLM\SOFTWARE\AnimeLogon; `keepRestore` keeps the recorded originals of a
// restore that failed, so a later install and uninstall can still put them back.
DWORD RemoveInstallRecord(bool keepRestore = false);

bool IsElevated();

}  // namespace animelogon::machine
