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
        if (n >= sizeof(WTSINFOEXW) && ex->Level == 1)
            s.locked = ex->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(raw);
    }
    return s;
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

Gate Decide(const GateInput &in) {
    if (!in.secure || !in.session.console) return Gate::Hold;
    if (in.shutdown) return Gate::Hold;
    // A session that had a user and now has none, still on the console, is a shutdown or
    // restart: signing out ends the session and draws the next logon screen in a new one.
    if (!in.session.signedIn && in.hadUser) return Gate::SignedOutInPlace;
    if (!in.session.signedIn || in.session.locked || in.lockNotified) return Gate::GoLive;
    return Gate::Hold;
}

ULONGLONG AwakeMs() {
    ULONGLONG unbiased = 0;
    QueryUnbiasedInterruptTime(&unbiased);
    return unbiased / 10000;
}

}  // namespace screen
