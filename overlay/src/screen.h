// What is on the Winlogon desktop, and whether it is a screen the video may cover.
//
// The secure desktop also carries UAC prompts, Ctrl+Alt+Del's options, the sign-out screen
// and Windows Update's shutdown screen. The video goes up only for a locked session or one
// nobody is signed in to, never while the machine is shutting down, and every doubtful case
// resolves to leaving the screen alone.
#pragma once

#include <windows.h>
#include <wtsapi32.h>

#include <string>

namespace screen {

// Is the Winlogon desktop the one on the glass?
bool InputDesktopIsSecure();

struct Session {
    bool console = false;   // this process's session is the console session
    bool signedIn = false;
    bool locked = false;
    // The session's connect state (WTS_CONNECTSTATE_CLASS). A lock screen is a session that is
    // Active; the logon screen at boot or after a sign-out is one that is Connected. Windows
    // Update's progress screen at boot is drawn while the session is still ConnectQuery, and a
    // shutdown that signed its user out leaves the session Down: neither is a screen to cover.
    // (Measured on Windows 11 26200 and Windows 10 19045 with real cumulative updates.)
    // -1 when it could not be read, which counts as at the logon stage: the gate then decides as
    // it did before it knew about connect states.
    int state = -1;
    bool AtLogonStage() const { return state == -1 || state == WTSActive || state == WTSConnected; }
};
Session ReadSession(DWORD session);

// The display LogonUI's credential window is on, if it has one up.
bool CredentialMonitor(RECT *monitor);

// Windows' shutdown screen is on this desktop.
bool ShutdownUnderway();

// The visible top-level windows of this desktop, topmost first (image, class, rectangle), for the
// log: what the video is about to cover, or did.
std::wstring DescribeDesktop(size_t most = 6);

// How the parked process decides to go live.
struct GateInput {
    bool secure = false;
    Session session;
    bool hadUser = false;          // somebody was signed in to this session earlier
    bool shutdown = false;         // ShutdownUnderway()
    bool goingDown = false;        // the launcher's events::kGoingDown is raised
    bool lockNotified = false;     // WTS_SESSION_LOCK arrived and no unlock since
};

enum class Gate {
    Hold,
    GoLive,
    SignedOutInPlace,  // a shutdown or restart signed the user out; never cover it
    GoingDown,         // Windows is shutting down or restarting; never cover it
    NotYet,            // the session is not at its logon screen yet (or any more): an update screen
};

// When to lower events::kGoingDown again: a shutdown somebody cancelled leaves them at their own
// unlocked desktop, which no shutdown, restart or update screen does. Twenty seconds of that and
// the flag goes, so the next lock is covered as usual.
class CancelWatch {
public:
    static constexpr ULONGLONG kCancelledMs = 20000;
    // True once, when the flag should be lowered.
    bool Update(bool goingDown, bool atDesktop, ULONGLONG now) {
        if (!goingDown || !atDesktop) {
            since_ = 0;
            return false;
        }
        if (!since_) since_ = now;
        if (now - since_ < kCancelledMs) return false;
        since_ = 0;
        return true;
    }

private:
    ULONGLONG since_ = 0;
};

// The session's lock flag can lag the desktop by seconds; the lock notification usually
// does not. A lag only delays the video: Ctrl+Alt+Del's options and UAC look the same.
Gate Decide(const GateInput &in);

// Milliseconds since boot, not counting time asleep.
ULONGLONG AwakeMs();

}  // namespace screen
