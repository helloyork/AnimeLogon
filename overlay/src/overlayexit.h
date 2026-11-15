// What overlay.exe's exit code tells the launcher, and whether the overlay the launcher starts
// next begins dismissed: parked, holding the video off the screen until the session is used,
// because the screen it would cover is one somebody is already signing in to.
//
// The rule everything here follows: rather show Windows' own sign-in than block somebody
// signing in. So the codes an overlay leaves with on purpose say exactly what they mean, and
// every other ending -- a crash, the watchdog, a code nobody planned for -- counts as dismissed:
// an overlay that died in the middle of a lock must not be replaced by one that covers the
// password box being typed into, and a crash that repeats must not freeze the screen again and
// again. The next lock, after the session has been used, shows the video as usual.
#pragma once

#include <windows.h>

namespace overlayexit {

// Exit codes overlay.exe chooses itself.
constexpr DWORD kNotWoken = 0;   // left without anybody having waved the screen away: the service
                                 // stopping, a lost graphics device, another overlay owning the desktop
constexpr DWORD kNoDevice = 2;   // no Direct3D device at the start: nothing was ever shown
constexpr DWORD kDismissed = 3;  // the screen it leaves had been waved away (also the watchdog's, and abort()'s)

// The code Windows ends a session's processes with when the session goes (a sign-out, a restart or
// a shutdown): DBG_TERMINATE_PROCESS.
constexpr DWORD kEndedWithSession = 0x40010004;

enum class Verdict {
    NotDismissed,  // the next overlay may cover the next logon screen at once
    Unchanged,     // nothing was shown: what the launcher believed before still holds
    Dismissed,     // the screen was waved away
    SessionEnded,  // Windows ended it with its session: dismissed, as an abnormal ending is
    Abnormal,      // a crash or another unplanned ending: dismissed too
};

inline Verdict Judge(DWORD code) {
    switch (code) {
    case kNotWoken: return Verdict::NotDismissed;
    case kNoDevice: return Verdict::Unchanged;
    case kDismissed: return Verdict::Dismissed;
    case kEndedWithSession: return Verdict::SessionEnded;
    default: return Verdict::Abnormal;
    }
}

// Whether the next overlay starts dismissed after one exited with `code`, when the launcher
// believed `before`.
inline bool StartsDismissed(DWORD code, bool before) {
    switch (Judge(code)) {
    case Verdict::NotDismissed: return false;
    case Verdict::Unchanged: return before;
    default: return true;
    }
}

// For the launcher's log.
inline const wchar_t *Describe(Verdict v) {
    switch (v) {
    case Verdict::NotDismissed: return L"the next one may cover the screen";
    case Verdict::Unchanged: return L"it never showed anything";
    case Verdict::Dismissed: return L"the screen had been waved away -- the next one starts dismissed";
    case Verdict::SessionEnded: return L"Windows ended it along with its session (a sign-out, restart or shutdown)";
    case Verdict::Abnormal: return L"an abnormal exit -- counted as dismissed, the next one starts dismissed";
    }
    return L"";
}

// When the console moves to another session -- "Switch user" on the password screen, or a
// sign-out, whose next logon screen is drawn in a new session -- the overlay in the old session
// is ended and one is started in the new. It starts dismissed if the old one was parked
// dismissed (`oldDismissed`, the overlays' events::kDismissed) and the old session still has its
// user (`oldKeepsItsUser`): that is "Switch user" -- somebody woke the screen and chose another
// account, and is signing in; the video must not come back between them and the password box.
// A sign-out takes the old session's user away (and ends the session), whatever an overlay
// started into it as it went said, so the logon screen that follows shows the video, as the
// first one after boot does.
inline bool SwitchStartsDismissed(bool oldDismissed, bool oldKeepsItsUser) { return oldDismissed && oldKeepsItsUser; }

}  // namespace overlayexit
