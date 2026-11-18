#include "animelogon/library.h"

#include <cwctype>
#include <mutex>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon::library {

namespace {

// The registry values that pin the library, under paths::kRegistryKey.
constexpr const wchar_t *kPathValue = L"Library";
constexpr const wchar_t *kVolumeValue = L"LibraryVolume";
constexpr const wchar_t *kIdValue = L"LibraryId";
constexpr const wchar_t *kInstallFolder = L"AnimeLogon";
constexpr const wchar_t *kLibraryFolder = L"Library";

struct Pinned {
    std::wstring path, volume, id;
    bool present() const { return !path.empty() && !volume.empty() && id.size() == 32; }
};

std::wstring ReadString(HKEY key, const wchar_t *name) {
    wchar_t text[512] = {};
    DWORD size = sizeof(text) - sizeof(wchar_t);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS) return {};
    return text;
}

bool IsVolumeName(const std::wstring &v) {
    // \\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\ as GetVolumeNameForVolumeMountPoint gives it.
    return v.size() == 49 && v.compare(0, 11, L"\\\\?\\Volume{") == 0 && v.compare(47, 2, L"}\\") == 0;
}

bool IsLibraryPath(const std::wstring &path) {
    return path.size() > 3 && IsDriveInstallDir(path.substr(0, path.size() - 8)) && path.compare(path.size() - 8, 8, L"\\Library") == 0;
}

Pinned ReadPin() {
    Pinned p;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, paths::kRegistryKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return p;
    p.path = ReadString(key, kPathValue);
    p.volume = ReadString(key, kVolumeValue);
    p.id = ReadString(key, kIdValue);
    RegCloseKey(key);
    // Only an administrator writes these; anything not spelled as Pin writes it is not a pin.
    if (!IsLibraryPath(p.path) || !IsVolumeName(p.volume) || p.id.size() != 32) return {};
    for (wchar_t c : p.id)
        if (!iswxdigit(c)) return {};
    return p;
}

// The pinned directory at its volume's current letter, or at the recorded one.
std::wstring Resolve(const Pinned &p) {
    wchar_t names[1024] = {};
    DWORD length = 0;
    if (GetVolumePathNamesForVolumeNameW(p.volume.c_str(), names, ARRAYSIZE(names), &length))
        for (const wchar_t *n = names; *n; n += wcslen(n) + 1)
            if (wcslen(n) == 3 && n[1] == L':' && n[2] == L'\\') return std::wstring(1, (wchar_t)towupper(n[0])) + p.path.substr(1);
    return p.path;
}

// The volume serial and 128-bit file id of a directory, as 32 hex digits of the id.
bool FileId(const std::wstring &dir, std::wstring *hex) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info));
    CloseHandle(h);
    if (!ok) return false;
    hex->clear();
    for (BYTE b : info.FileId.Identifier) *hex += Format(L"%02x", b);
    return true;
}

// Dir() is asked for on every path into the library; the registry and the mount manager are
// asked at most every two seconds.
std::mutex g_lock;
std::wstring g_test;
std::wstring g_dir;
bool g_elsewhere = false;
ULONGLONG g_at = 0;

void Refresh() {
    const ULONGLONG now = GetTickCount64();
    if (g_at && now - g_at < 2000) return;
    const Pinned p = ReadPin();
    g_elsewhere = p.present();
    g_dir = g_elsewhere ? Resolve(p) : paths::DataDir();
    g_at = now;
}

void Forget() {
    std::lock_guard<std::mutex> hold(g_lock);
    g_at = 0;
}

}  // namespace

std::wstring Dir() {
    if (!paths::Sandbox().empty()) return paths::DataDir();
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_test.empty()) return g_test;
    Refresh();
    return g_dir;
}

bool Elsewhere() {
    if (!paths::Sandbox().empty()) return false;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_test.empty()) return !EqualsNoCase(g_test, paths::DataDir());
    Refresh();
    return g_elsewhere;
}

bool Verify(std::wstring *why) {
    if (!paths::Sandbox().empty()) return true;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_test.empty()) return true;
    }
    const Pinned p = ReadPin();
    if (!p.present()) return true;
    const std::wstring dir = Resolve(p), root = dir.substr(0, 3), install = dir.substr(0, dir.size() - 8);
    wchar_t volume[64] = {};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, ARRAYSIZE(volume)) || !EqualsNoCase(volume, p.volume)) {
        *why = root + L" is not the drive the library was installed on";
        return false;
    }
    std::wstring id;
    if (!FileId(dir, &id)) {
        *why = dir + L" cannot be opened (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    if (!EqualsNoCase(id, p.id)) {
        *why = dir + L" is not the directory installed as the library";
        return false;
    }
    std::wstring w;
    if (!secure::IsTrustedDirectory(install, &w)) {
        *why = install + L" " + w;
        return false;
    }
    if (secure::RootIsOpen(root, &w)) {
        *why = root + L" lets " + w + L" remove what is in it";
        return false;
    }
    return true;
}

std::wstring InstallDirOn(wchar_t letter) {
    return std::wstring(1, (wchar_t)towupper(letter)) + L":\\" + kInstallFolder;
}

std::wstring LibraryIn(const std::wstring &installDir) { return installDir + L"\\" + kLibraryFolder; }

bool IsDriveInstallDir(const std::wstring &dir) {
    return dir.size() == 3 + wcslen(kInstallFolder) && dir[0] >= L'A' && dir[0] <= L'Z' && dir == InstallDirOn(dir[0]);
}

DWORD Pin(const std::wstring &dir) {
    if (!IsLibraryPath(dir)) return ERROR_INVALID_PARAMETER;
    wchar_t volume[64] = {};
    if (!GetVolumeNameForVolumeMountPointW(dir.substr(0, 3).c_str(), volume, ARRAYSIZE(volume))) return GetLastError();
    std::wstring id;
    if (!FileId(dir, &id)) return GetLastError();
    HKEY key = nullptr;
    DWORD e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, paths::kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                              nullptr);
    if (e != ERROR_SUCCESS) return e;
    for (const auto &[name, value] : {std::pair<const wchar_t *, std::wstring>{kPathValue, dir}, {kVolumeValue, volume}, {kIdValue, id}}) {
        e = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        if (e != ERROR_SUCCESS) break;
    }
    RegCloseKey(key);
    Forget();
    return e;
}

DWORD Unpin() {
    HKEY key = nullptr;
    DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, paths::kRegistryKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    for (const wchar_t *name : {kPathValue, kVolumeValue, kIdValue}) {
        const DWORD d = RegDeleteValueW(key, name);
        if (d != ERROR_SUCCESS && d != ERROR_FILE_NOT_FOUND) e = d;
    }
    RegCloseKey(key);
    Forget();
    return e;
}

Refusal Judge(const DriveFacts &d) {
    if (d.system) return Refusal::None;
    if (d.type != DRIVE_FIXED) return Refusal::NotLocal;
    if (d.removableBus) return Refusal::Removable;
    if (!d.readable) return Refusal::Locked;
    if ((!EqualsNoCase(d.fileSystem, L"NTFS") && !EqualsNoCase(d.fileSystem, L"ReFS")) || !d.persistentAcls)
        return Refusal::NoPermissions;
    if (d.lockedAtBoot) return Refusal::LockedAtBoot;
    if (d.rootOpen) return Refusal::OpenRoot;
    return Refusal::None;
}

void UseForTests(const std::wstring &dir) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_test = dir;
    g_at = 0;
}

}  // namespace animelogon::library
