// launcher.exe: the AnimeLogon service. Runs as SYSTEM in session 0 and keeps one overlay
// process on the console session's WinSta0\Winlogon desktop, where the logon screen is. The
// overlay parks itself there until a logon screen is actually up, so a lock costs only a
// little composition rather than starting a process from cold.
//
// Run with --console to supervise from a command window instead of as a service.

#include <windows.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <string>

#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/paths.h"

#include "events.h"
#include "livedata.h"
#include "overlayexit.h"
#include "sessionrecord.h"
#include "winlogonwatch.h"

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
std::atomic<bool> g_stop{false};
HANDLE g_stopEvent = nullptr;
// Raised while Windows shuts down or restarts (events.h); overlays stay off the screen.
HANDLE g_goingDownEvent = nullptr;
// Raised by an overlay parked dismissed (events.h), read when the console moves to another session.
HANDLE g_dismissedEvent = nullptr;
// Set by the service handler after a resume from sleep or hibernation, handled by Supervise.
std::atomic<bool> g_resumed{false};
// Set once the settings app or uninstaller has asked for the sign-in background setting back.
std::atomic<bool> g_restored{false};
constexpr ULONGLONG kSignInCheckMs = 60000;
// More time passed by the clock than the machine was awake: it slept or hibernated in between.
constexpr ULONGLONG kAsleepMs = 5000;
// The longest the going-down flag stays up without the pre-shutdown phase following. Pre-shutdown
// came 1 to 2 minutes after Winlogon's request on every update shutdown measured; Windows
// Update's own servicing runs after it, with the service already stopped. While the flag is up,
// the gate's other rules still apply when it is lowered: a session Windows signed out in place,
// or one not at its logon screen, is not covered either way.
constexpr ULONGLONG kGoingDownMaxMs = 10 * 60 * 1000;

std::wstring OverlayPath() { return animelogon::paths::ModuleDir() + L"overlay.exe"; }

// The overlay of the session being left was parked dismissed (events::kDismissed).
bool OverlayDismissed() { return g_dismissedEvent && WaitForSingleObject(g_dismissedEvent, 0) == WAIT_OBJECT_0; }

// Creates the overlays' events afresh: nothing is going down, nothing has been waved away. The
// first logon screen after the service starts shows the video.
void CreateEvents() {
    g_stopEvent = events::Create(events::kStop);
    ResetEvent(g_stopEvent);
    g_goingDownEvent = events::Create(events::kGoingDown);
    ResetEvent(g_goingDownEvent);
    g_dismissedEvent = events::Create(events::kDismissed);
    ResetEvent(g_dismissedEvent);
}

void SetState(DWORD state, DWORD wait = 0) {
    g_status.dwCurrentState = state;
    // Pre-shutdown comes once the signed-in user is gone and while Windows Update installs, so the
    // overlay is off the screen before Windows' progress screen comes up; plain shutdown comes only
    // after that phase is over, minutes later.
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                                                SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_POWEREVENT
                                                          : 0;
    g_status.dwWaitHint = wait;
    g_status.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
                                ? g_status.dwCheckPoint + 1
                                : 0;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

DWORD FindWinlogon(DWORD session) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"winlogon.exe") != 0) continue;
            DWORD sid = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid == session) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Somebody is signed in to `session`.
bool SignedIn(DWORD session) {
    if (session == 0xFFFFFFFF) return false;
    LPWSTR user = nullptr;
    DWORD n = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &n)) return false;
    const bool in = user && user[0];
    WTSFreeMemory(user);
    return in;
}

// Spawns overlay.exe onto WinSta0\Winlogon in `session`, by borrowing winlogon's token.
// `hadUser`: somebody has been signed in to the session (sessionrecord.h).
HANDLE SpawnOverlay(DWORD session, bool dismissed, bool hadUser) {
    const DWORD winlogon = FindWinlogon(session);
    if (!winlogon) return nullptr;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogon);
    if (!proc) return nullptr;
    HANDLE token = nullptr, primary = nullptr, child = nullptr;
    if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &token) &&
        DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
        SetTokenInformation(primary, TokenSessionId, &session, sizeof(session));
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<wchar_t *>(L"WinSta0\\Winlogon");
        std::wstring cmd = L"\"" + OverlayPath() + L"\"";
        if (dismissed) cmd += L" --dismissed";
        if (hadUser) cmd += L" --had-user";
        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, primary, FALSE);
        PROCESS_INFORMATION pi{};
        if (CreateProcessAsUserW(primary, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi)) {
            ALOG(L"launcher: overlay pid %lu on WinSta0\\Winlogon in session %lu", pi.dwProcessId, session);
            CloseHandle(pi.hThread);
            child = pi.hProcess;
        } else {
            ALOG(L"launcher: CreateProcessAsUser failed (%lu)", GetLastError());
        }
        if (env) DestroyEnvironmentBlock(env);
    }
    if (primary) CloseHandle(primary);
    if (token) CloseHandle(token);
    CloseHandle(proc);
    return child;
}

// Keeps one overlay alive in the console session. Returns when the stop event is set.
void Supervise() {
    animelogon::instance::Guard guard;
    if (guard.Acquire(animelogon::instance::kSupervisor, true) == animelogon::instance::Result::AlreadyRunning) {
        ALOG(L"launcher: another supervisor is already running -- exiting");
        return;
    }
    HANDLE child = nullptr;
    DWORD childSession = 0xFFFFFFFF;
    ULONGLONG nextTry = 0;
    DWORD backoff = 0;
    ULONGLONG spawnedAt = 0;
    bool dismissed = false;  // the next overlay starts parked until the screen is used
    bool saidPaused = false;
    ULONGLONG signInCheckedAt = 0;
    // What the signed-in person's session reports for the components: what is playing.
    LiveData live;
    live.Start();

    // Winlogon's word that the machine is going down (winlogonwatch.h).
    winlogonwatch::Watch winlogon;
    winlogon.Start([](DWORD) {
        if (g_goingDownEvent && WaitForSingleObject(g_goingDownEvent, 0) != WAIT_OBJECT_0) {
            SetEvent(g_goingDownEvent);
            ALOG(L"launcher: Windows is going down -- overlays stay off the screen");
        }
    });
    // The console session's history, and a handle to its winlogon.exe, which is signalled when the
    // session ends: session ids are handed out again.
    sessionrecord::Record record;
    HANDLE sessionWinlogon = nullptr;
    ULONGLONG flushedAt = 0;
    // A resume is also seen without the power event: the tick count runs on through sleep and
    // hibernation, the unbiased interrupt time does not. Fast startup is such a resume: the
    // service is hibernated with the kernel instead of being stopped, so whatever it believed
    // about the shutdown it was in the middle of has to be dropped when it wakes.
    ULONGLONG lastTick = GetTickCount64(), lastUnbiased = 0;
    QueryUnbiasedInterruptTime(&lastUnbiased);
    // When the going-down flag went up, to lower one the pre-shutdown phase never followed.
    ULONGLONG goingDownSince = 0;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        const DWORD session = WTSGetActiveConsoleSessionId();
        const ULONGLONG now = GetTickCount64();

        ULONGLONG unbiased = 0;
        QueryUnbiasedInterruptTime(&unbiased);
        const ULONGLONG awake = (unbiased - lastUnbiased) / 10000, passed = now - lastTick;
        lastTick = now;
        lastUnbiased = unbiased;
        if (passed > awake + kAsleepMs && !g_resumed.exchange(true))
            ALOG(L"launcher: %llu s passed while the machine was asleep or hibernated", (passed - awake) / 1000);

        if (g_resumed.exchange(false)) {
            record.Forget();
            if (g_goingDownEvent) ResetEvent(g_goingDownEvent);
            // One resume arrives as several notices (the power events and the gap in the clock).
            static ULONGLONG resumeSaidAt = 0;
            if (!resumeSaidAt || now - resumeSaidAt > 10000)
                ALOG(L"launcher: resumed from sleep or hibernation -- the session history is cleared");
            resumeSaidAt = now;
        }
        // A shutdown that never reached the pre-shutdown phase (cancelled, or a fast-startup
        // shutdown the resume was not noticed after) must not keep the video away for good.
        const bool goingDown = g_goingDownEvent && WaitForSingleObject(g_goingDownEvent, 0) == WAIT_OBJECT_0;
        if (!goingDown) {
            goingDownSince = 0;
        } else if (!goingDownSince) {
            goingDownSince = now;
        } else if (now - goingDownSince > kGoingDownMaxMs) {
            ResetEvent(g_goingDownEvent);
            goingDownSince = 0;
            ALOG(L"launcher: Windows said it was going down %llu min ago and is still here -- the flag is lowered",
                 kGoingDownMaxMs / 60000);
        }
        const bool ended = sessionWinlogon && WaitForSingleObject(sessionWinlogon, 0) == WAIT_OBJECT_0;
        // The session the console is leaving, read before the record moves on: "Switch user"
        // leaves it signed in, a sign-out does not.
        const DWORD leaving = record.Session();
        const bool leavingKeepsItsUser = leaving != 0xFFFFFFFF && leaving != session && !ended && SignedIn(leaving);
        switch (record.Update(session, SignedIn(session), ended)) {
        case sessionrecord::Record::Change::NewSession: {
            if (sessionWinlogon) CloseHandle(sessionWinlogon);
            sessionWinlogon = nullptr;
            if (const DWORD pid = session != 0xFFFFFFFF ? FindWinlogon(session) : 0)
                sessionWinlogon = OpenProcess(SYNCHRONIZE, FALSE, pid);
            // The overlay of the new session starts dismissed only after "Switch user" from a
            // screen somebody had waved away (overlayexit.h).
            const bool carried = overlayexit::SwitchStartsDismissed(OverlayDismissed(), leavingKeepsItsUser);
            if (leaving != 0xFFFFFFFF && session != 0xFFFFFFFF)
                ALOG(L"launcher: the console moved from session %lu to %lu (%s) -- the overlay there starts %s", leaving,
                     session, leavingKeepsItsUser ? L"the one before still signed in: switch user" : L"the one before signed out or ended",
                     carried ? L"dismissed, as the screen before had been waved away" : L"ready to cover the screen");
            dismissed = carried;
            break;
        }
        case sessionrecord::Record::Change::EmptiedInPlace:
            ALOG(L"launcher: session %lu's user was signed out and the session has not ended -- a shutdown "
                 L"or restart; no overlay is started in it",
                 session);
            break;
        default:
            break;
        }

        // Windows hides the background behind the credential screen for any account whose
        // setting says so; accounts appear and settings change, so this is checked again.
        if (!g_restored.load() && (!signInCheckedAt || now - signInCheckedAt > kSignInCheckMs)) {
            signInCheckedAt = now;
            const DWORD e = animelogon::machine::ShowSignInBackground();
            if (e != ERROR_SUCCESS) ALOG(L"launcher: the sign-in background setting could not be changed (%lu)", e);
        }

        // The console moved: the overlay of the session left behind goes (whether the next starts
        // dismissed was decided above, as the record saw the new session).
        if (child && session != 0xFFFFFFFF && session != childSession && childSession != 0xFFFFFFFF) {
            TerminateProcess(child, 0);
            WaitForSingleObject(child, 2000);
            CloseHandle(child);
            child = nullptr;
            nextTry = 0;
        }
        if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            const ULONGLONG lived = now - spawnedAt;
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            // Anything but the endings the overlay chooses -- a crash, the watchdog, a code nobody
            // planned for -- counts as dismissed, so the next overlay never covers a password box
            // being typed into; the next lock shows the video again.
            const overlayexit::Verdict verdict = overlayexit::Judge(code);
            dismissed = overlayexit::StartsDismissed(code, dismissed);
            ALOG(L"launcher: overlay exited (%lu, 0x%08lX) after %llu ms -- %s", code, code, lived,
                 overlayexit::Describe(verdict));
            CloseHandle(child);
            child = nullptr;
            // A child that exits quickly and repeatedly is a broken build: back off.
            backoff = lived < 3000 ? (backoff ? (backoff * 2 > 30000 ? 30000 : backoff * 2) : 1000) : 0;
            nextTry = now + backoff;
        }
        // Paused from the logon screen: nothing runs until the settings app resumes it.
        const bool paused = !child && animelogon::machine::Paused();
        if (paused != saidPaused) {
            saidPaused = paused;
            if (paused) ALOG(L"launcher: paused -- not starting the overlay");
        }
        if (!child && !paused && session != 0xFFFFFFFF && now >= nextTry && !record.EmptiedInPlace()) {
            child = SpawnOverlay(session, dismissed, record.HadUser());
            childSession = child ? session : 0xFFFFFFFF;
            spawnedAt = now;
            if (!child) nextTry = now + (backoff = backoff ? (backoff * 2 > 30000 ? 30000 : backoff * 2) : 2000);
        }
        live.Tick(session);
        if (now - flushedAt >= 250) {
            winlogon.Flush();
            flushedAt = now;
        }
        WaitForSingleObject(g_stopEvent, 100);
    }
    live.Stop();
    winlogon.Stop();
    if (sessionWinlogon) CloseHandle(sessionWinlogon);

    if (child) {
        // Ask the overlay to leave, then wait briefly before ending it.
        WaitForSingleObject(child, 2000);
        if (WaitForSingleObject(child, 0) != WAIT_OBJECT_0) TerminateProcess(child, 0);
        CloseHandle(child);
    }
}

DWORD WINAPI HandlerEx(DWORD control, DWORD eventType, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_PRESHUTDOWN:
        // Windows Update installs in this phase, behind its own progress screen. Take the overlay
        // off the screen and stop now; a service that accepts pre-shutdown and does not stop holds
        // the shutdown up for the service manager's whole pre-shutdown timeout.
        ALOG(L"launcher: pre-shutdown -- taking the overlay off the screen and stopping");
        if (g_goingDownEvent) SetEvent(g_goingDownEvent);
        SetState(SERVICE_STOP_PENDING, 5000);
        g_stop = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetState(SERVICE_STOP_PENDING, 5000);
        g_stop = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        if (eventType == PBT_APMRESUMEAUTOMATIC || eventType == PBT_APMRESUMESUSPEND) g_resumed = true;
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    case animelogon::machine::kServiceControlRestore: {
        g_restored = true;
        const DWORD e = animelogon::machine::RestoreSignInBackground();
        ALOG(L"launcher: sign-in background setting restored (%lu)", e);
        return NO_ERROR;
    }
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(animelogon::paths::kServiceName, HandlerEx, nullptr);
    if (!g_statusHandle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    // Started only to put the sign-in background setting back, which needs SYSTEM.
    if (argc >= 2 && _wcsicmp(argv[1], animelogon::machine::kServiceRestoreArg) == 0) {
        SetState(SERVICE_START_PENDING, 3000);
        const DWORD e = animelogon::machine::RestoreSignInBackground();
        ALOG(L"launcher: started to restore the sign-in background setting (%lu)", e);
        SetState(SERVICE_STOPPED);
        return;
    }
    SetState(SERVICE_START_PENDING, 3000);
    CreateEvents();
    SetState(SERVICE_RUNNING);
    Supervise();
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_goingDownEvent) {
        CloseHandle(g_goingDownEvent);
        g_goingDownEvent = nullptr;
    }
    if (g_dismissedEvent) {
        CloseHandle(g_dismissedEvent);
        g_dismissedEvent = nullptr;
    }
    SetState(SERVICE_STOPPED);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    animelogon::log::Open(animelogon::paths::LogPath(L"launcher.log"));
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--console") == 0) {
            ALOG(L"launcher: supervising from the console (Ctrl+C to stop)");
            CreateEvents();
            Supervise();
            return 0;
        }
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(animelogon::paths::kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD e = GetLastError();
        if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            ALOG(L"launcher: not started as a service; run with --console to supervise here");
        else
            ALOG(L"launcher: StartServiceCtrlDispatcher failed (%lu)", e);
        return (int)e;
    }
    return 0;
}
