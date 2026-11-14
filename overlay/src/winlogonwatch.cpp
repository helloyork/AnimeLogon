#include "winlogonwatch.h"

#include <evntcons.h>

#include <cstring>
#include <vector>

#include "animelogon/log.h"

namespace winlogonwatch {
namespace {

// Microsoft-Windows-Winlogon.
constexpr GUID kWinlogon = {0xdbe9b383, 0x7cf3, 0x4331, {0x91, 0xcc, 0xa3, 0xcb, 0x16, 0xa3, 0xb5, 0x38}};
// A session name is machine-wide and outlives a process that was killed, so Start stops any
// session of this name before opening its own.
constexpr wchar_t kSession[] = L"AnimeLogon.Winlogon";
constexpr USHORT kShutdownRequested = 6101;

std::vector<BYTE> Properties() {
    std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSession) + 16, 0);
    auto *p = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC time stamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->FlushTimer = 1;           // seconds: the shortest a real-time session hands buffers over
    p->BufferSize = 16;          // KB; Winlogon writes a few events a minute at most
    p->MinimumBuffers = 2;
    p->MaximumBuffers = 4;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buf;
}

}  // namespace

bool IsShutdownRequest(DWORD flags) { return (flags & (EWX_SHUTDOWN | EWX_REBOOT | EWX_POWEROFF)) != 0; }

void WINAPI Watch::OnRecord(PEVENT_RECORD record) {
    auto *self = static_cast<Watch *>(record->UserContext);
    if (!self || !IsEqualGUID(record->EventHeader.ProviderId, kWinlogon)) return;
    self->events_.fetch_add(1);
    if (record->EventHeader.EventDescriptor.Id != kShutdownRequested) return;
    if (!record->UserData || record->UserDataLength < sizeof(DWORD)) {
        ALOG(L"winlogon: event 6101 without its flags (%u bytes)", (unsigned)record->UserDataLength);
        return;
    }
    DWORD flags = 0;
    std::memcpy(&flags, record->UserData, sizeof(flags));
    const bool down = IsShutdownRequest(flags);
    ALOG(L"winlogon: event 6101, flags 0x%08lX -- %s", flags, down ? L"a shutdown or restart" : L"a sign-out");
    if (down && self->onShutdown_) self->onShutdown_(flags);
}

DWORD WINAPI Watch::Pump(LPVOID p) {
    auto *self = static_cast<Watch *>(p);
    TRACEHANDLE h = self->consumer_;
    const ULONG e = ProcessTrace(&h, 1, nullptr, nullptr);
    if (e != ERROR_SUCCESS && e != ERROR_CANCELLED) ALOG(L"winlogon: ProcessTrace ended (%lu)", e);
    return 0;
}

bool Watch::Start(std::function<void(DWORD)> onShutdown) {
    Stop();
    onShutdown_ = std::move(onShutdown);
    std::vector<BYTE> props = Properties();
    ControlTraceW(0, kSession, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()), EVENT_TRACE_CONTROL_STOP);
    props = Properties();
    ULONG e = StartTraceW(&session_, kSession, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()));
    if (e != ERROR_SUCCESS) {
        ALOG(L"winlogon: StartTrace failed (%lu) -- shutdowns are known only from the service manager", e);
        session_ = 0;
        return false;
    }
    e = EnableTraceEx2(session_, &kWinlogon, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, ~0ULL, 0, 0,
                       nullptr);
    if (e != ERROR_SUCCESS) {
        ALOG(L"winlogon: EnableTraceEx2 failed (%lu)", e);
        Stop();
        return false;
    }
    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<wchar_t *>(kSession);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = &Watch::OnRecord;
    log.Context = this;
    consumer_ = OpenTraceW(&log);
    if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
        ALOG(L"winlogon: OpenTrace failed (%lu)", GetLastError());
        Stop();
        return false;
    }
    thread_ = CreateThread(nullptr, 0, &Watch::Pump, this, 0, nullptr);
    if (!thread_) {
        ALOG(L"winlogon: no thread (%lu)", GetLastError());
        Stop();
        return false;
    }
    ALOG(L"winlogon: listening for shutdown requests");
    return true;
}

void Watch::Stop() {
    if (session_) {
        std::vector<BYTE> props = Properties();
        ControlTraceW(session_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES *>(props.data()),
                      EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (consumer_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(consumer_);
        consumer_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

}  // namespace winlogonwatch
