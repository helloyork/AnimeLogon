#include "animelogon/log.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon::log {
namespace {

constexpr unsigned kMaxLines = 20000;
constexpr unsigned kRetryLines = 100;

constexpr ULONGLONG kTrustRecheckMs = 60 * 1000;

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring *g_path = nullptr;
bool g_counted = false;
unsigned g_lines = 0;
unsigned g_nextCheck = 0;
bool g_privileged = false;
int g_trusted = 0;  // 1 yes, -1 no, 0 not checked yet
ULONGLONG g_trustCheckedAt = 0;

// SYSTEM, or an elevated administrator.
bool IsPrivileged() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool privileged = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                      elevation.TokenIsElevated;
    alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    if (!privileged && GetTokenInformation(token, TokenUser, user, sizeof(user), &size))
        privileged = IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(user)->User.Sid, WinLocalSystemSid) != FALSE;
    CloseHandle(token);
    return privileged;
}

std::wstring DirOf(const std::wstring &path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Called with g_lock held. A privileged process writes only into a directory no other
// account can change. Checked once, then at most once a minute while it fails; the
// debugger hears about the first refusal, not every line.
bool MayWriteLocked() {
    if (!g_privileged || g_trusted > 0) return true;
    const ULONGLONG now = GetTickCount64();
    if (g_trusted < 0 && now - g_trustCheckedAt < kTrustRecheckMs) return false;
    g_trustCheckedAt = now;
    std::wstring why;
    const bool ok = secure::IsTrustedDirectory(DirOf(*g_path), &why);
    if (!ok && g_trusted == 0)
        OutputDebugStringW((L"AnimeLogon: not logging to " + *g_path + L": its directory " + why + L"\r\n").c_str());
    g_trusted = ok ? 1 : -1;
    return ok;
}

// The log file itself, refusing a link in its place.
HANDLE OpenLogFile(const wchar_t *path, DWORD access, DWORD disposition) {
    HANDLE h = CreateFileW(path, access | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi) ||
        (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || fi.nNumberOfLinks != 1) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

unsigned CountNewlines(const char *p, DWORD n) {
    unsigned c = 0;
    for (DWORD i = 0; i < n; ++i)
        if (p[i] == '\n') ++c;
    return c;
}

unsigned CountLines(const wchar_t *path) {
    HANDLE h = OpenLogFile(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[8192];
    unsigned lines = 0;
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) lines += CountNewlines(buf, got);
    CloseHandle(h);
    return lines;
}

// Called with g_lock held. Several processes may share one file, so a full count is
// re-read from disk before rotating.
void AppendLocked(const std::string &bytes) {
    if (!g_path || !MayWriteLocked()) return;
    const wchar_t *path = g_path->c_str();
    if (!g_counted) {
        g_lines = CountLines(path);
        g_counted = true;
    }
    const unsigned threshold = g_nextCheck ? g_nextCheck : kMaxLines;
    if (g_lines >= threshold) {
        const unsigned actual = CountLines(path);
        const std::wstring previous = *g_path + L".1";
        if (actual < kMaxLines) {
            g_lines = actual;
            g_nextCheck = 0;
        } else if (MoveFileExW(path, previous.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            g_lines = 0;
            g_nextCheck = 0;
        } else {
            g_lines = actual;
            g_nextCheck = actual + kRetryLines;
        }
    }
    HANDLE h = OpenLogFile(path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr))
        g_lines += CountNewlines(bytes.data(), written);
    CloseHandle(h);
}

std::string FormatLine(const wchar_t *format, va_list args) {
    const std::wstring body = FormatV(format, args);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring line =
        Format(L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %s\r\n", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(),
               body.c_str());
    OutputDebugStringW(line.c_str());
    return ToUtf8(line);
}

}  // namespace

void Open(const std::wstring &path) {
    // The machine log directory is the installer's to create: made here by a privileged
    // process, it would get whatever its parent hands down.
    const bool privileged = IsPrivileged();
    if (!privileged) paths::CreateDirectories(DirOf(path));
    AcquireSRWLockExclusive(&g_lock);
    if (!g_path) g_path = new std::wstring();
    *g_path = path;
    g_counted = false;
    g_nextCheck = 0;
    g_privileged = privileged;
    g_trusted = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void Write(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    const std::string line = FormatLine(format, args);
    va_end(args);
    AcquireSRWLockExclusive(&g_lock);
    AppendLocked(line);
    ReleaseSRWLockExclusive(&g_lock);
}

bool TryWrite(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    const std::string line = FormatLine(format, args);
    va_end(args);
    if (!TryAcquireSRWLockExclusive(&g_lock)) return false;
    AppendLocked(line);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

}  // namespace animelogon::log
