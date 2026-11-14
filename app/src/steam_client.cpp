#include "steam_client.h"

#include <cwchar>
#include <memory>

#include "animelogon/log.h"
#include "animelogon/text.h"

#include "appid.h"

using namespace animelogon;

namespace app::steam {

std::wstring HelperIn(const std::wstring &programDir) {
    std::wstring dir = programDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return L"";
    const std::wstring path = dir + L"\\steam\\animelogon-steam.exe";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return L"";
    return path;
}

uint32_t AppId() {
    wchar_t text[16] = L"";
    const DWORD n = GetEnvironmentVariableW(L"ANIMELOGON_STEAM_APPID", text, ARRAYSIZE(text));
    if (n > 0 && n < ARRAYSIZE(text)) {
        unsigned long long v = 0;
        bool digits = true;
        for (DWORD i = 0; i < n; ++i) {
            if (text[i] < L'0' || text[i] > L'9') digits = false;
            else v = v * 10 + (text[i] - L'0');
        }
        if (digits && v > 0 && v <= 0xFFFFFFFFull) return (uint32_t)v;
    }
    return steamhelper::kAppId;
}

bool IsItemId(std::wstring_view text) {
    if (text.empty() || text.size() > 20 || text[0] == L'0') return false;
    unsigned long long v = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const unsigned digit = c - L'0';
        if (v > (0xFFFFFFFFFFFFFFFFull - digit) / 10) return false;
        v = v * 10 + digit;
    }
    return true;
}

// --- lines -------------------------------------------------------------------------------------

void Lines::Feed(const char *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char c = bytes[i];
        if (c == '\n') {
            if (skipping_) ++dropped_;
            else {
                if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
                ready_.push_back(std::move(partial_));
            }
            partial_.clear();
            skipping_ = false;
        } else if (!skipping_) {
            if (partial_.size() >= limit_) {
                skipping_ = true;
                partial_.clear();
                partial_.shrink_to_fit();
            } else {
                partial_ += c;
            }
        }
    }
}

bool Lines::Next(std::string *line) {
    if (next_ >= ready_.size()) {
        ready_.clear();
        next_ = 0;
        return false;
    }
    *line = std::move(ready_[next_++]);
    return true;
}

// --- the helper --------------------------------------------------------------------------------

Client::Client(std::wstring helper, uint32_t appid, std::function<void()> wake)
    : helper_(std::move(helper)), appid_(appid), wake_(std::move(wake)) {}

Client::~Client() { Stop(2000); }

bool Client::Start() {
    if (process_ && !exited_) return true;
    if (process_) Stop(0);  // gone by itself: tidy up before starting again
    if (helper_.empty()) return false;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 64 * 1024)) return false;
    if (!CreatePipe(&outRead, &outWrite, &sa, 64 * 1024)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    // Only the child's ends are handed down, and only to it.
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    // It writes to standard error only for a wrong command line; that goes nowhere.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRead;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = nul != INVALID_HANDLE_VALUE ? nul : outWrite;
    HANDLE inherit[3] = {inRead, outWrite, nul};
    const DWORD inherited = nul != INVALID_HANDLE_VALUE ? 3 : 2;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::unique_ptr<uint8_t[]> list(new uint8_t[bytes]);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.get());
    bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) &&
                  UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                            inherited * sizeof(HANDLE), nullptr, nullptr);
    if (listed) si.lpAttributeList = attributes;

    std::wstring command = L"\"" + helper_ + L"\"";
    if (appid_) command += L" --appid " + std::to_wstring(appid_);
    std::wstring folder = helper_.substr(0, helper_.find_last_of(L'\\'));
    PROCESS_INFORMATION pi{};
    const bool started = CreateProcessW(helper_.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                        folder.c_str(), &si.StartupInfo, &pi);
    const DWORD error = started ? 0 : GetLastError();
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        ALOG(L"steam: %s could not start (%lu)", helper_.c_str(), error);
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    in_ = inWrite;
    exited_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gone_ = false;
        lines_.clear();
    }
    reader_ = std::thread([this, outRead] { Read(outRead); });
    ALOG(L"steam: helper started (pid %lu, app %u)", pi.dwProcessId, appid_);
    return true;
}

void Client::Read(HANDLE out) {
    Lines lines;
    static constexpr DWORD kChunk = 64 * 1024;
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    DWORD got = 0;
    while (ReadFile(out, buffer.get(), kChunk, &got, nullptr) && got > 0) {
        lines.Feed(buffer.get(), got);
        std::vector<json::Value> parsed;
        std::string line;
        while (lines.Next(&line)) {
            json::Value v;
            if (json::Parse(FromUtf8(line), &v) && v.IsObject()) parsed.push_back(std::move(v));
            else ALOG(L"steam: a line that is not a JSON object (%zu bytes)", line.size());
        }
        if (parsed.empty()) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (json::Value &v : parsed) lines_.push_back(std::move(v));
        }
        if (wake_) wake_();
    }
    CloseHandle(out);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gone_ = true;
    }
    if (wake_) wake_();
}

bool Client::Write(const std::string &bytes) {
    if (!in_) return false;
    DWORD wrote = 0;
    return WriteFile(in_, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
}

long long Client::Request(std::wstring_view op, const std::function<void(json::Writer &)> &params) {
    if (!Start()) return 0;
    const long long id = ++last_;
    json::Writer w;
    w.BeginObject().Key(L"id").Int(id).Member(L"op", op);
    if (params) params(w);
    w.EndObject();
    if (!Write(ToUtf8(w.text()) + "\n")) {
        ALOG(L"steam: the helper does not take requests (%lu)", GetLastError());
        return 0;
    }
    return id;
}

std::vector<json::Value> Client::Take(bool *exited) {
    std::vector<json::Value> out;
    bool gone = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(lines_);
        gone = gone_;
    }
    if (gone && !exited_) {
        exited_ = true;
        DWORD code = 0;
        if (process_ && WaitForSingleObject(process_, 200) == WAIT_OBJECT_0) GetExitCodeProcess(process_, &code);
        ALOG(L"steam: helper exited (%lu)", code);
    }
    if (exited) *exited = gone;
    return out;
}

void Client::Stop(DWORD waitMs) {
    if (in_) {
        if (!exited_ && waitMs) Write("{\"id\":" + std::to_string(++last_) + ",\"op\":\"quit\"}\n");
        CloseHandle(in_);
        in_ = nullptr;
    }
    if (process_) {
        if (WaitForSingleObject(process_, waitMs) != WAIT_OBJECT_0) {
            ALOG(L"steam: helper did not exit; ending it");
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 2000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    exited_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    gone_ = false;
}

}  // namespace app::steam
