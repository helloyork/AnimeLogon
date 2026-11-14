// What the launcher remembers about the console session, to tell a shutdown from a logon screen.
//
// Signing out ends the session: the next logon screen is drawn in a new one. A shutdown or restart
// started from a signed-in session signs its user out and keeps the session, still the console,
// drawing "Shutting down", "Restarting" or Windows Update's progress until the power goes. To the
// overlay that reads exactly like the logon screen at boot: nobody signed in, the logon desktop up.
// So a session that has had somebody signed in and now has nobody, while it is still the console
// and has not ended, gets no new overlay, and every overlay started into a session that has had a
// user is told so (--had-user), so it holds itself in the same case.
#pragma once

#include <windows.h>

namespace sessionrecord {

class Record {
public:
    enum class Change {
        None,
        NewSession,      // a different console session, or the same id after the session ended
        UserSeen,        // somebody is signed in to it, for the first time in this record
        EmptiedInPlace,  // its user was signed out and the session did not end
    };

    // One reading: the console session id, whether somebody is signed in to it, and whether the
    // session this record is about has ended (its winlogon.exe exited) since the last reading.
    Change Update(DWORD console, bool signedIn, bool ended) {
        if (console != session_ || ended) {
            session_ = console;
            hadUser_ = signedIn;
            emptied_ = false;
            return Change::NewSession;
        }
        if (signedIn) {
            emptied_ = false;
            if (!hadUser_) {
                hadUser_ = true;
                return Change::UserSeen;
            }
            return Change::None;
        }
        // A sign-out also reads as nobody for a moment (100-200 ms) before the console moves to
        // the new session; holding off a start for that long costs nothing.
        if (hadUser_ && !emptied_) {
            emptied_ = true;
            return Change::EmptiedInPlace;
        }
        return Change::None;
    }

    // After a resume from sleep or hibernation: fast startup hibernates instead of ending the
    // boot, and the session that comes back may be a fresh logon screen.
    void Forget() {
        session_ = kNone;
        hadUser_ = emptied_ = false;
    }

    DWORD Session() const { return session_; }
    bool HadUser() const { return hadUser_; }
    // No overlay may be started into the session.
    bool EmptiedInPlace() const { return emptied_; }

private:
    static constexpr DWORD kNone = 0xFFFFFFFF;
    DWORD session_ = kNone;
    bool hadUser_ = false;
    bool emptied_ = false;
};

}  // namespace sessionrecord
