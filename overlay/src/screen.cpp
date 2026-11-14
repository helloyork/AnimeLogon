#include "screen.h"

#include <wtsapi32.h>

#include <cwchar>

namespace screen {

bool InputDesktopIsSecure() {
    HDESK in = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!in) return false;
    wchar_t name[64] = L"";
    DWORD need = 0;
    const bool ok = GetUserObjectInformationW(in, UOI_NAME, name, sizeof(name), &need) != 0;
    CloseDesktop(in);
    return ok && _wcsicmp(name, L"Winlogon") == 0;
}

Session ReadSession(DWORD session) {
    Session s;
    const DWORD console = WTSGetActiveConsoleSessionId();
    s.console = console != 0xFFFFFFFF && console == session;
    LPWSTR user = nullptr;
    DWORD n = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &n)) {
        s.signedIn = user && user[0];
        WTSFreeMemory(user);
    }
    LPWSTR raw = nullptr;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &raw, &n) && raw) {
        const auto *ex = reinterpret_cast<const WTSINFOEXW *>(raw);
        if (n >= sizeof(WTSINFOEXW) && ex->Level == 1) {
            s.locked = ex->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
            s.state = static_cast<int>(ex->Data.WTSInfoExLevel1.SessionState);
        }
        WTSFreeMemory(raw);
    }
    return s;
}

bool CredentialMonitor(RECT *monitor) {
    HWND best = nullptr;
    LONGLONG bestArea = 0;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        RECT r{};
        GetWindowRect(w, &r);
        const LONGLONG area = (LONGLONG)(r.right - r.left) * (r.bottom - r.top);
        if (area <= bestArea) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) continue;
        wchar_t image[MAX_PATH] = L"";
        DWORD n = ARRAYSIZE(image);
        const bool ok = QueryFullProcessImageNameW(p, 0, image, &n) != FALSE;
        CloseHandle(p);
        const wchar_t *name = wcsrchr(image, L'\\');
        if (ok && name && _wcsicmp(name + 1, L"LogonUI.exe") == 0) {
            best = w;
            bestArea = area;
        }
    }
    MONITORINFO mi{sizeof(mi)};
    if (!best || !GetMonitorInfoW(MonitorFromWindow(best, MONITOR_DEFAULTTONULL), &mi)) return false;
    *monitor = mi.rcMonitor;
    return true;
}

bool ShutdownUnderway() {
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) return true;
    const DWORD self = GetCurrentProcessId();
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == self) continue;
        wchar_t cls[64] = L"";
        // Only ever a reason to stay off the screen, so a renamed class fails safe.
        if (GetClassNameW(w, cls, ARRAYSIZE(cls)) && _wcsnicmp(cls, L"BlockedShutdownUX", 17) == 0) return true;
    }
    return false;
}

std::wstring DescribeDesktop(size_t most) {
    std::wstring out;
    size_t n = 0;
    const DWORD self = GetCurrentProcessId();
    for (HWND w = GetTopWindow(nullptr); w && n < most; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == self) continue;
        RECT r{};
        GetWindowRect(w, &r);
        if (r.right <= r.left || r.bottom <= r.top) continue;
        wchar_t cls[64] = L"";
        GetClassNameW(w, cls, ARRAYSIZE(cls));
        wchar_t image[MAX_PATH] = L"?";
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD len = ARRAYSIZE(image);
            if (!QueryFullProcessImageNameW(p, 0, image, &len)) wcscpy_s(image, L"?");
            CloseHandle(p);
        }
        const wchar_t *name = wcsrchr(image, L'\\');
        wchar_t line[256];
        swprintf_s(line, L"%s[%s '%s' %ld,%ld %ldx%ld]", out.empty() ? L"" : L" ", name ? name + 1 : image, cls, r.left,
                   r.top, r.right - r.left, r.bottom - r.top);
        out += line;
        ++n;
    }
    return out.empty() ? L"(nothing visible)" : out;
}

Gate Decide(const GateInput &in) {
    if (!in.secure || !in.session.console) return Gate::Hold;
    if (in.goingDown) return Gate::GoingDown;
    if (in.shutdown) return Gate::Hold;
    // A session that had a user and now has none, still on the console, is a shutdown or
    // restart: signing out ends the session and draws the next logon screen in a new one.
    if (!in.session.signedIn && in.hadUser) return Gate::SignedOutInPlace;
    const bool logonScreen = !in.session.signedIn || in.session.locked || in.lockNotified;
    if (logonScreen && !in.session.AtLogonStage()) return Gate::NotYet;
    if (logonScreen) return Gate::GoLive;
    return Gate::Hold;
}

ULONGLONG AwakeMs() {
    ULONGLONG unbiased = 0;
    QueryUnbiasedInterruptTime(&unbiased);
    return unbiased / 10000;
}

}  // namespace screen
