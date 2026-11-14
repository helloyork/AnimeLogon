// Named events the launcher raises for the overlays it starts. SYSTEM only.
#pragma once

#include <windows.h>
#include <sddl.h>

namespace events {

// The service is stopping: leave the screen and exit.
constexpr const wchar_t *kStop = L"Global\\AnimeLogon.Stop";

// Windows is shutting down or restarting: stay off the screen, parked or live, and stay alive.
// On the logon desktop a shutdown, a restart and Windows Update's progress screens all look like a
// logon screen nobody is signed in to, so nothing an overlay can read for itself tells them apart
// in time. The launcher raises this when Winlogon asks for a shutdown (its event 6101) and when the
// service manager starts the pre-shutdown phase, in which Windows Update installs; an overlay
// raises it on WM_ENDSESSION. It is lowered after a resume from sleep or hibernation, and by an
// overlay that sees the shutdown cancelled (somebody back at an unlocked desktop).
constexpr const wchar_t *kGoingDown = L"Global\\AnimeLogon.GoingDown";

// Manual-reset, SYSTEM and Administrators only.
inline HANDLE Create(const wchar_t *name) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd,
                                                        nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE h = CreateEventW(sd ? &sa : nullptr, TRUE, FALSE, name);
    if (sd) LocalFree(sd);
    return h;
}

inline bool IsSet(const wchar_t *name) {
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!h) return false;
    const bool set = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return set;
}

// Raises or lowers an event the launcher created. False if it does not exist (no launcher).
inline bool Signal(const wchar_t *name, bool on) {
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!h) return false;
    const bool ok = (on ? SetEvent(h) : ResetEvent(h)) != FALSE;
    CloseHandle(h);
    return ok;
}

}  // namespace events
