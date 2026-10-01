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

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
std::atomic<bool> g_stop{false};
HANDLE g_stopEvent = nullptr;

std::wstring OverlayPath() { return animelogon::paths::ModuleDir() + L"overlay.exe"; }

// overlay.exe's exit code when the screen it leaves was already waved away.
constexpr DWORD kOverlayExitDismissed = 3;

void SetState(DWORD state, DWORD wait = 0) {
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
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

// Spawns overlay.exe onto WinSta0\Winlogon in `session`, by borrowing winlogon's token.
HANDLE SpawnOverlay(DWORD session, bool dismissed) {
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

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        const DWORD session = WTSGetActiveConsoleSessionId();
        const ULONGLONG now = GetTickCount64();

        if (child && session != 0xFFFFFFFF && session != childSession && childSession != 0xFFFFFFFF) {
            TerminateProcess(child, 0);
            WaitForSingleObject(child, 2000);
            CloseHandle(child);
            child = nullptr;
            nextTry = 0;
            dismissed = false;
        }
        if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
            const ULONGLONG lived = now - spawnedAt;
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            ALOG(L"launcher: overlay exited (%lu) after %llu ms", code, lived);
            dismissed = code == kOverlayExitDismissed;
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
        if (!child && !paused && session != 0xFFFFFFFF && now >= nextTry) {
            child = SpawnOverlay(session, dismissed);
            childSession = child ? session : 0xFFFFFFFF;
            spawnedAt = now;
            if (!child) nextTry = now + (backoff = backoff ? (backoff * 2 > 30000 ? 30000 : backoff * 2) : 2000);
        }
        WaitForSingleObject(g_stopEvent, 100);
    }

    if (child) {
        // Ask the overlay to leave, then wait briefly before ending it.
        WaitForSingleObject(child, 2000);
        if (WaitForSingleObject(child, 0) != WAIT_OBJECT_0) TerminateProcess(child, 0);
        CloseHandle(child);
    }
}

DWORD WINAPI HandlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetState(SERVICE_STOP_PENDING, 5000);
        g_stop = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR *) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(animelogon::paths::kServiceName, HandlerEx, nullptr);
    if (!g_statusHandle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    SetState(SERVICE_START_PENDING, 3000);
    g_stopEvent = events::Create(events::kStop);
    ResetEvent(g_stopEvent);
    SetState(SERVICE_RUNNING);
    Supervise();
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    SetState(SERVICE_STOPPED);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    animelogon::log::Open(animelogon::paths::LogPath(L"launcher.log"));
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--console") == 0) {
            ALOG(L"launcher: supervising from the console (Ctrl+C to stop)");
            g_stopEvent = events::Create(events::kStop);
            ResetEvent(g_stopEvent);
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
