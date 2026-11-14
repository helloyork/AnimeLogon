// Hears Winlogon ask for a shutdown or restart, for the launcher.
//
// On the logon desktop a shutdown, a restart and Windows Update's progress screens look like a
// logon screen nobody is signed in to. GetSystemMetrics(SM_SHUTTINGDOWN) stays 0 there, no window
// of Windows' shutdown UI appears, and WM_ENDSESSION does not reach a window that is already up
// over the logon screen. What does say so, about a second after the request and long before the
// power goes, is Winlogon's own event 6101, whose payload starts with the ExitWindowsEx flags.
// This runs a real-time ETW session on the Winlogon provider and reports that event.
//
// It lives in the launcher rather than the overlay because the launcher is a service that runs
// from boot until the very end of a shutdown: an overlay started late, or replaced, would have
// missed the event.
#pragma once

#include <windows.h>
#include <evntrace.h>

#include <atomic>
#include <functional>

namespace winlogonwatch {

// Event 6101's flags ask for the machine to go down (shut down, restart, power off), not for a
// sign-out, which is EWX_LOGOFF, zero.
bool IsShutdownRequest(DWORD flags);

class Watch {
public:
    Watch() = default;
    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;
    ~Watch() { Stop(); }

    // `onShutdown` is called on the session's own thread with the flags of each 6101 that asks
    // for a shutdown or restart; every 6101 is logged. False if the session could not be
    // started; the caller keeps working without it.
    bool Start(std::function<void(DWORD flags)> onShutdown);
    void Stop();

    // How many Winlogon events arrived, for the log.
    long Events() const { return events_.load(); }

private:
    static void WINAPI OnRecord(PEVENT_RECORD record);
    static DWORD WINAPI Pump(LPVOID self);

    std::function<void(DWORD)> onShutdown_;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    HANDLE thread_ = nullptr;
    std::atomic<long> events_{0};
};

}  // namespace winlogonwatch
