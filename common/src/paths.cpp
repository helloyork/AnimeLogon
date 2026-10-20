#include "animelogon/paths.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <cwchar>
#include <string_view>

namespace animelogon::paths {

static std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t *fallback) {
    wchar_t *base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &base)) && base)
        dir = base;
    else
        dir = fallback;
    CoTaskMemFree(base);
    return dir;
}

std::wstring DataDir() {
    static const std::wstring dir =
        KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData") + L"\\AnimeLogon";
    return dir;
}

std::wstring LogDir() { return DataDir() + L"\\logs"; }
std::wstring LogPath(const wchar_t *name) { return LogDir() + L"\\" + name; }
std::wstring SettingsPath() { return DataDir() + L"\\settings.ini"; }
std::wstring BackgroundPath() { return DataDir() + L"\\background.png"; }
std::wstring PausedMarkerPath() { return DataDir() + L"\\paused"; }

std::wstring UserDataDir() {
    return KnownFolder(FOLDERID_LocalAppData, L"") + L"\\AnimeLogon";
}

std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    std::wstring path(buf, n < ARRAYSIZE(buf) ? n : 0);
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

bool CreateDirectories(const std::wstring &path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 2) CreateDirectories(path.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool IsPlainAbsolute(const std::wstring &path) {
    if (path.size() < 3 || path.size() > 32000) return false;
    const wchar_t drive = path[0];
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) || path[1] != L':' ||
        path[2] != L'\\')
        return false;
    if (path.size() == 3) return true;
    size_t start = 3;
    while (start <= path.size()) {
        size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring_view part = std::wstring_view(path).substr(start, end - start);
        if (part.empty() || part == L"." || part == L"..") return false;
        for (wchar_t c : part)
            if (c < 32 || wcschr(L"/:*?\"<>|", c)) return false;
        // Windows drops a trailing dot or space, which would give one file two names.
        if (part.back() == L'.' || part.back() == L' ') return false;
        start = end + 1;
    }
    return true;
}

bool IsWithin(const std::wstring &path, const std::wstring &root) {
    if (!IsPlainAbsolute(path) || !IsPlainAbsolute(root) || path.size() < root.size()) return false;
    if (CompareStringOrdinal(path.c_str(), (int)root.size(), root.c_str(), (int)root.size(), TRUE) != CSTR_EQUAL)
        return false;
    return path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\';
}

}  // namespace animelogon::paths
