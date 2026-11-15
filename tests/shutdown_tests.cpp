// Keeping off Windows' shutdown, restart and update screens: the parked overlay's gate, the
// launcher's record of the console session, when a shutdown counts as cancelled, and which of
// Winlogon's shutdown requests count.
#include "check.h"

#include "screen.h"
#include "sessionrecord.h"
#include "winlogonwatch.h"

using screen::CancelWatch;
using screen::Decide;
using screen::Gate;
using screen::GateInput;
using sessionrecord::Record;

namespace {

GateInput LogonDesktop(bool signedIn, bool locked) {
    GateInput in;
    in.secure = true;
    in.session.console = true;
    in.session.signedIn = signedIn;
    in.session.locked = locked;
    return in;
}

}  // namespace

TEST(GateCoversTheBootLogonScreenAndALock) {
    CHECK(Decide(LogonDesktop(false, false)) == Gate::GoLive);
    CHECK(Decide(LogonDesktop(true, true)) == Gate::GoLive);
    GateInput notified = LogonDesktop(true, false);
    notified.lockNotified = true;
    CHECK(Decide(notified) == Gate::GoLive);
}

TEST(GateLeavesUacAndOtherSecureDesktopsAlone) {
    // Signed in, unlocked, no lock notification: a consent prompt or Ctrl+Alt+Del.
    CHECK(Decide(LogonDesktop(true, false)) == Gate::Hold);
    GateInput off = LogonDesktop(false, false);
    off.secure = false;
    CHECK(Decide(off) == Gate::Hold);
    GateInput other = LogonDesktop(false, false);
    other.session.console = false;
    CHECK(Decide(other) == Gate::Hold);
}

TEST(GateNeverCoversAShutdown) {
    // From the desktop: the session keeps its id and loses its user.
    GateInput out = LogonDesktop(false, false);
    out.hadUser = true;
    CHECK(Decide(out) == Gate::SignedOutInPlace);
    // Winlogon or the service manager said so: whatever the session looks like.
    for (bool signedIn : {false, true}) {
        for (bool locked : {false, true}) {
            GateInput in = LogonDesktop(signedIn, locked);
            in.goingDown = true;
            in.lockNotified = locked;
            CHECK(Decide(in) == Gate::GoingDown);
        }
    }
    GateInput shut = LogonDesktop(false, false);
    shut.shutdown = true;
    CHECK(Decide(shut) == Gate::Hold);
}

TEST(GateWaitsForTheSessionToReachItsLogonScreen) {
    // Readings from Windows 11 26200 with a real cumulative update, 2026-11-14.
    GateInput update = LogonDesktop(false, false);  // "You're 19% there", first boot after
    update.session.state = WTSConnectQuery;
    CHECK(Decide(update) == Gate::NotYet);
    GateInput down = LogonDesktop(false, false);  // the shutdown after alt was signed out
    down.session.state = WTSDown;
    CHECK(Decide(down) == Gate::NotYet);
    GateInput boot = LogonDesktop(false, false);  // the real logon screen after the update
    boot.session.state = WTSConnected;
    CHECK(Decide(boot) == Gate::GoLive);
    GateInput lock = LogonDesktop(true, true);  // a lock
    lock.session.state = WTSActive;
    CHECK(Decide(lock) == Gate::GoLive);
    GateInput consent = LogonDesktop(true, false);  // UAC: Active and unlocked
    consent.session.state = WTSActive;
    CHECK(Decide(consent) == Gate::Hold);
    // A state that could not be read decides as before.
    CHECK(Decide(LogonDesktop(false, false)) == Gate::GoLive);
}

TEST(SessionRecordSeesASignOutInPlace) {
    Record r;
    CHECK(r.Update(1, false, false) == Record::Change::NewSession);  // boot logon screen
    CHECK(!r.HadUser() && !r.EmptiedInPlace());
    CHECK(r.Update(1, true, false) == Record::Change::UserSeen);
    CHECK(r.HadUser());
    CHECK(r.Update(1, true, false) == Record::Change::None);
    // "Update and shut down" from the desktop: same session, nobody signed in.
    CHECK(r.Update(1, false, false) == Record::Change::EmptiedInPlace);
    CHECK(r.EmptiedInPlace());
    CHECK(r.Update(1, false, false) == Record::Change::None);
    CHECK(r.EmptiedInPlace());
}

TEST(SessionRecordStartsAgainWithEachSession) {
    Record r;
    r.Update(1, true, false);
    r.Update(1, false, false);  // a sign-out reads as nobody for a moment
    CHECK(r.EmptiedInPlace());
    // ... and then the console is a new session with the next logon screen.
    CHECK(r.Update(2, false, false) == Record::Change::NewSession);
    CHECK(!r.HadUser() && !r.EmptiedInPlace());
    // The same id again after the session ended (its winlogon.exe exited).
    r.Update(2, true, false);
    r.Update(2, false, false);
    CHECK(r.Update(2, false, true) == Record::Change::NewSession);
    CHECK(!r.EmptiedInPlace());
}

TEST(SessionRecordKeepsTheSessionLeftWhileTheConsoleHasNone) {
    // "Switch user": the console is session 2, belongs to none for a few milliseconds, then is 3.
    constexpr DWORD kNone = 0xFFFFFFFF;
    Record r;
    r.Update(2, true, false);
    CHECK(r.Update(kNone, false, false) == Record::Change::None);
    CHECK(r.Session() == 2 && r.HadUser());  // still the session being left, read before the next update
    CHECK(r.Update(3, false, false) == Record::Change::NewSession);
    CHECK(r.Session() == 3);
    // After a resume the record knows no session; a reading of none still changes nothing.
    r.Forget();
    CHECK(r.Update(kNone, false, false) == Record::Change::None);
    CHECK(r.Update(3, false, false) == Record::Change::NewSession);
}

TEST(SessionRecordForgetsAfterAResume) {
    Record r;
    r.Update(1, true, false);
    r.Update(1, false, false);
    r.Forget();
    CHECK(!r.HadUser() && !r.EmptiedInPlace());
    CHECK(r.Update(1, false, false) == Record::Change::NewSession);
    CHECK(!r.EmptiedInPlace());
}

TEST(SessionRecordUserBackClearsTheHold) {
    Record r;
    r.Update(1, true, false);
    r.Update(1, false, false);
    CHECK(r.Update(1, true, false) == Record::Change::None);
    CHECK(!r.EmptiedInPlace() && r.HadUser());
}

TEST(ShutdownCountsAsCancelledAfterTwentySecondsAtTheDesktop) {
    CancelWatch w;
    CHECK(!w.Update(true, true, 1000));
    CHECK(!w.Update(true, true, 1000 + CancelWatch::kCancelledMs - 1));
    CHECK(w.Update(true, true, 1000 + CancelWatch::kCancelledMs));
    // Leaving the desktop starts the count again; so does not going down at all.
    CancelWatch v;
    CHECK(!v.Update(true, true, 0));
    CHECK(!v.Update(true, false, 10000));
    CHECK(!v.Update(true, true, 15000));
    CHECK(!v.Update(true, true, 15000 + CancelWatch::kCancelledMs - 1));
    CHECK(v.Update(true, true, 15000 + CancelWatch::kCancelledMs));
    CancelWatch u;
    CHECK(!u.Update(false, true, 0));
    CHECK(!u.Update(false, true, 100000));
}

TEST(WinlogonShutdownRequestsAreShutdownsRestartsAndPowerOffs) {
    using winlogonwatch::IsShutdownRequest;
    CHECK(!IsShutdownRequest(EWX_LOGOFF));
    CHECK(!IsShutdownRequest(EWX_LOGOFF | EWX_FORCE));
    CHECK(IsShutdownRequest(EWX_SHUTDOWN));
    CHECK(IsShutdownRequest(EWX_REBOOT));
    CHECK(IsShutdownRequest(EWX_POWEROFF));
    // The flags Windows passes for a restart asked for from a live logon screen: 0x20007.
    CHECK(IsShutdownRequest(0x20007));
}
