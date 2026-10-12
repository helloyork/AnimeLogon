#include "livedata.h"

#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include "animelogon/live.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"

namespace {

using namespace animelogon;

constexpr DWORD kNoSession = 0xFFFFFFFF;
constexpr ULONGLONG kSettingsMs = 5000;  // how often settings.ini's `media` is read again
constexpr ULONGLONG kWriteMs = 250;      // live.ini is written at most this often
// Interactive users may connect and write; nobody else, and nobody may make another instance.
constexpr const wchar_t *kPipeSddl = L"D:P(A;;GA;;;SY)(A;;0x12019f;;;IU)";

std::wstring AgentPath() { return paths::ModuleDir() + L"animelogon.exe"; }

std::vector<unsigned char> SidOf(HANDLE token) {
    std::vector<unsigned char> buffer(256);
    DWORD need = 0;
    if (!GetTokenInformation(token, TokenUser, buffer.data(), (DWORD)buffer.size(), &need)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
        buffer.resize(need);
        if (!GetTokenInformation(token, TokenUser, buffer.data(), (DWORD)buffer.size(), &need)) return {};
    }
    PSID sid = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
    const DWORD length = GetLengthSid(sid);
    const auto *bytes = static_cast<const unsigned char *>(sid);
    return {bytes, bytes + length};
}

}  // namespace

bool LiveData::Start() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE pipe = CreateNamedPipeW(live::kPipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
                                   (DWORD)live::kMaxBytes, 0, &sa);
    const DWORD e = GetLastError();
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) {
        ALOG(L"live: cannot make the pipe (%lu) -- nothing playing is shown", e);
        return false;
    }
    // Nothing from before the service started belongs to anybody now.
    DeleteFileW(live::Path().c_str());
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pipe_ = pipe;
    thread_ = std::thread([this] { Serve(); });
    return true;
}

void LiveData::Stop() {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    if (stop_) CloseHandle(stop_);
    stop_ = nullptr;
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    EndAgent();
    Forget(nullptr);
}

void LiveData::Forget(const wchar_t *why) {
    std::lock_guard<std::mutex> hold(lock_);
    allowed_ = kNoSession;
    userSid_.clear();
    if (DeleteFileW(live::Path().c_str()) && why) ALOG(L"live: live.ini deleted -- %s", why);
}

void LiveData::EndAgent() {
    if (!agent_) return;
    TerminateProcess(agent_, 0);
    WaitForSingleObject(agent_, 2000);
    CloseHandle(agent_);
    agent_ = nullptr;
    agentSession_ = kNoSession;
}

void LiveData::Tick(DWORD consoleSession) {
    const ULONGLONG now = GetTickCount64();
    if (!settingsAt_ || now - settingsAt_ >= kSettingsMs) {
        settingsAt_ = now;
        const bool media = LoadSettings(false).media;
        if (media != media_) ALOG(L"live: what is playing is %s", media ? L"shown again" : L"no longer shown");
        media_ = media;
    }
    HANDLE token = nullptr;
    const bool signedIn = consoleSession != kNoSession && WTSQueryUserToken(consoleSession, &token);
    const std::vector<unsigned char> sid = signedIn ? SidOf(token) : std::vector<unsigned char>{};
    const bool want = signedIn && media_ && !sid.empty();

    if (agent_ && WaitForSingleObject(agent_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(agent_, &code);
        const ULONGLONG lived = now - agentAt_;
        ALOG(L"live: the agent exited (%lu) after %llu ms", code, lived);
        CloseHandle(agent_);
        agent_ = nullptr;
        agentSession_ = kNoSession;
        // One that ends quickly and again and again is broken: back off.
        backoff_ = lived < 10000 ? (backoff_ ? (backoff_ * 2 > 300000 ? 300000 : backoff_ * 2) : 5000) : 0;
        nextTry_ = now + backoff_;
        Forget(L"the agent ended");
    }
    bool sameUser = false;
    {
        std::lock_guard<std::mutex> hold(lock_);
        sameUser = sid == userSid_;
    }
    if (agent_ && (!want || agentSession_ != consoleSession || !sameUser)) {
        EndAgent();
        backoff_ = 0;
        nextTry_ = 0;
    }
    if (!want && allowed_ != kNoSession)
        Forget(!signedIn ? L"nobody is signed in at the console" : L"settings.ini says media = false");

    if (want && !agent_ && now >= nextTry_) {
        {
            std::lock_guard<std::mutex> hold(lock_);
            allowed_ = consoleSession;
            userSid_ = sid;
        }
        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, token, FALSE);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<wchar_t *>(L"WinSta0\\Default");
        std::wstring cmd = L"\"" + AgentPath() + L"\" --agent";
        PROCESS_INFORMATION pi{};
        if (CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, env, nullptr,
                                 &si, &pi)) {
            ALOG(L"live: agent pid %lu in session %lu", pi.dwProcessId, consoleSession);
            CloseHandle(pi.hThread);
            agent_ = pi.hProcess;
            agentSession_ = consoleSession;
            agentAt_ = now;
        } else {
            ALOG(L"live: cannot start the agent (%lu)", GetLastError());
            backoff_ = backoff_ ? (backoff_ * 2 > 300000 ? 300000 : backoff_ * 2) : 5000;
            nextTry_ = now + backoff_;
        }
        if (env) DestroyEnvironmentBlock(env);
    }
    if (token) CloseHandle(token);
}

bool LiveData::Trusted(HANDLE pipe, DWORD *session) {
    ULONG pid = 0, clientSession = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !GetNamedPipeClientSessionId(pipe, &clientSession)) return false;
    std::vector<unsigned char> sid;
    {
        std::lock_guard<std::mutex> hold(lock_);
        if (allowed_ == kNoSession || clientSession != allowed_) return false;
        sid = userSid_;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH * 2];
    DWORD length = ARRAYSIZE(image);
    bool ok = QueryFullProcessImageNameW(process, 0, image, &length) && _wcsicmp(image, AgentPath().c_str()) == 0;
    HANDLE token = nullptr;
    ok = ok && OpenProcessToken(process, TOKEN_QUERY, &token) && SidOf(token) == sid;
    if (token) CloseHandle(token);
    CloseHandle(process);
    if (ok) *session = clientSession;
    return ok;
}

void LiveData::Write(const std::string &message, DWORD session) {
    live::Live l;
    std::wstring why;
    if (!live::Parse(message, &l, &why)) {
        ALOG(L"live: a message was refused -- %s", why.c_str());
        return;
    }
    std::lock_guard<std::mutex> hold(lock_);
    if (allowed_ != session) return;  // the console has changed hands since
    l.session = session;
    const std::string bytes = live::Serialize(l, true);
    const DWORD e = secure::WriteBytes(live::Path(), bytes.data(), bytes.size(), secure::kPrivateFileSddl);
    if (e != ERROR_SUCCESS) ALOG(L"live: cannot write live.ini (%lu)", e);
}

void LiveData::Serve() {
    HANDLE pipe = pipe_;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<char> buffer(live::kMaxBytes);
    // Waits for the overlapped call in progress; false when the service is stopping.
    auto wait = [&] {
        HANDLE both[] = {ov.hEvent, stop_};
        if (WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0) return true;
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return false;
    };
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD done = 0;
        BOOL ok = ConnectNamedPipe(pipe, &ov);
        DWORD e = ok ? ERROR_SUCCESS : GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (!wait()) break;
            ok = GetOverlappedResult(pipe, &ov, &done, FALSE);
            e = ok ? ERROR_SUCCESS : GetLastError();
        }
        if (e != ERROR_SUCCESS && e != ERROR_PIPE_CONNECTED) {
            DisconnectNamedPipe(pipe);
            if (WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        DWORD session = kNoSession;
        if (!Trusted(pipe, &session)) {
            ALOG(L"live: a connection that is not the console's agent was refused");
            DisconnectNamedPipe(pipe);
            continue;
        }
        ULONGLONG wrote = 0;
        bool stopping = false;
        for (;;) {
            ResetEvent(ov.hEvent);
            ok = ReadFile(pipe, buffer.data(), (DWORD)buffer.size(), &done, &ov);
            e = ok ? ERROR_SUCCESS : GetLastError();
            if (e == ERROR_IO_PENDING) {
                if (!wait()) {
                    stopping = true;
                    break;
                }
                ok = GetOverlappedResult(pipe, &ov, &done, FALSE);
                e = ok ? ERROR_SUCCESS : GetLastError();
            }
            if (e == ERROR_MORE_DATA) ALOG(L"live: a message longer than %zu bytes was refused", live::kMaxBytes);
            if (e != ERROR_SUCCESS) break;  // the agent went away, or broke the rules
            const ULONGLONG since = GetTickCount64() - wrote;
            if (wrote && since < kWriteMs && WaitForSingleObject(stop_, (DWORD)(kWriteMs - since)) == WAIT_OBJECT_0) {
                stopping = true;
                break;
            }
            Write(std::string(buffer.data(), done), session);
            wrote = GetTickCount64();
        }
        DisconnectNamedPipe(pipe);
        // What a gone agent reported is no longer known to be true.
        {
            std::lock_guard<std::mutex> hold(lock_);
            DeleteFileW(live::Path().c_str());
        }
        if (stopping) break;
    }
    CloseHandle(ov.hEvent);
}
