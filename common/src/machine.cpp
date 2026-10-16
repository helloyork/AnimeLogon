#include "animelogon/machine.h"

#include <vector>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/text.h"

namespace animelogon::machine {
namespace {

constexpr const wchar_t *kPolicyKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization";
constexpr const wchar_t *kCspKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP";
constexpr const wchar_t *kSystemPolicyKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\System";
constexpr const wchar_t *kRestoreKey = L"SOFTWARE\\AnimeLogon\\Restore";
constexpr const wchar_t *kProtectedKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SystemProtectedUserData";
constexpr const wchar_t *kHideValue = L"HideLogonBackgroundImage";
constexpr const wchar_t *kSignInTag = L"SignIn.";

struct Tracked {
    const wchar_t *key;
    const wchar_t *value;
    const wchar_t *tag;
};

const Tracked kTracked[] = {
    {kPolicyKey, L"NoLockScreen", L"Personalization.NoLockScreen"},
    {kCspKey, L"LockScreenImagePath", L"PersonalizationCSP.LockScreenImagePath"},
    {kCspKey, L"LockScreenImageUrl", L"PersonalizationCSP.LockScreenImageUrl"},
    {kCspKey, L"LockScreenImageStatus", L"PersonalizationCSP.LockScreenImageStatus"},
};

struct Value {
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
};

Value ReadValue(const wchar_t *key, const wchar_t *name) {
    Value v;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return v;
    DWORD size = 0;
    if (RegQueryValueExW(h, name, nullptr, &v.type, nullptr, &size) == ERROR_SUCCESS) {
        v.data.resize(size);
        if (RegQueryValueExW(h, name, nullptr, &v.type, v.data.data(), &size) == ERROR_SUCCESS) {
            v.data.resize(size);
            v.present = true;
        }
    }
    RegCloseKey(h);
    return v;
}

DWORD WriteValue(const wchar_t *key, const wchar_t *name, DWORD type, const void *data, DWORD size) {
    HKEY h = nullptr;
    DWORD e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &h,
                              nullptr);
    if (e != ERROR_SUCCESS) return e;
    e = RegSetValueExW(h, name, 0, type, static_cast<const BYTE *>(data), size);
    RegCloseKey(h);
    return e;
}

DWORD DeleteValue(const wchar_t *key, const wchar_t *name) {
    HKEY h = nullptr;
    DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h);
    if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return e;
    e = RegDeleteValueW(h, name);
    RegCloseKey(h);
    return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
}

DWORD WriteString(const wchar_t *key, const wchar_t *name, const std::wstring &text) {
    return WriteValue(key, name, REG_SZ, text.c_str(), (DWORD)((text.size() + 1) * sizeof(wchar_t)));
}

DWORD WriteDword(const wchar_t *key, const wchar_t *name, DWORD value) {
    return WriteValue(key, name, REG_DWORD, &value, sizeof(value));
}

std::wstring AsString(const Value &v) {
    if (!v.present || (v.type != REG_SZ && v.type != REG_EXPAND_SZ)) return {};
    std::wstring s(reinterpret_cast<const wchar_t *>(v.data.data()), v.data.size() / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

bool AsDword(const Value &v, DWORD *out) {
    if (!v.present || v.type != REG_DWORD || v.data.size() < sizeof(DWORD)) return false;
    *out = *reinterpret_cast<const DWORD *>(v.data.data());
    return true;
}

// The keys holding those values. One AnimeLogon had to create is deleted again on restore,
// once it is empty.
struct TrackedKey {
    const wchar_t *key;
    const wchar_t *tag;
};

const TrackedKey kTrackedKeys[] = {
    {kPolicyKey, L"Personalization"},
    {kCspKey, L"PersonalizationCSP"},
};

// The original value is recorded once; later changes by AnimeLogon do not overwrite it.
DWORD Remember(const Tracked &t) {
    const std::wstring marker = std::wstring(t.tag) + L".Recorded";
    DWORD recorded = 0;
    if (AsDword(ReadValue(kRestoreKey, marker.c_str()), &recorded) && recorded) return ERROR_SUCCESS;
    const Value v = ReadValue(t.key, t.value);
    const DWORD e = v.present ? WriteValue(kRestoreKey, t.tag, v.type, v.data.data(), (DWORD)v.data.size())
                              : DeleteValue(kRestoreKey, t.tag);
    return e == ERROR_SUCCESS ? WriteDword(kRestoreKey, marker.c_str(), 1) : e;
}

DWORD RememberKey(const TrackedKey &k) {
    const std::wstring marker = std::wstring(k.tag) + L".KeyExisted";
    if (ReadValue(kRestoreKey, marker.c_str()).present) return ERROR_SUCCESS;
    HKEY h = nullptr;
    const LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, k.key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h);
    if (e == ERROR_SUCCESS) RegCloseKey(h);
    else if (e != ERROR_FILE_NOT_FOUND) return (DWORD)e;
    return WriteDword(kRestoreKey, marker.c_str(), e == ERROR_SUCCESS ? 1 : 0);
}

DWORD RestoreKey(const TrackedKey &k) {
    const std::wstring marker = std::wstring(k.tag) + L".KeyExisted";
    DWORD existed = 1;
    if (!AsDword(ReadValue(kRestoreKey, marker.c_str()), &existed)) return ERROR_SUCCESS;
    if (!existed) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k.key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
            DWORD subkeys = 0, values = 0;
            const LSTATUS q = RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                               nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(h);
            if (q == ERROR_SUCCESS && !subkeys && !values) {
                const LSTATUS e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, k.key, KEY_WOW64_64KEY, 0);
                if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return (DWORD)e;
            }
        }
    }
    return DeleteValue(kRestoreKey, marker.c_str());
}

DWORD Restore(const Tracked &t) {
    const std::wstring marker = std::wstring(t.tag) + L".Recorded";
    DWORD recorded = 0;
    if (!AsDword(ReadValue(kRestoreKey, marker.c_str()), &recorded) || !recorded) return ERROR_SUCCESS;
    const Value original = ReadValue(kRestoreKey, t.tag);
    const DWORD e = original.present
                        ? WriteValue(t.key, t.value, original.type, original.data.data(), (DWORD)original.data.size())
                        : DeleteValue(t.key, t.value);
    if (e == ERROR_SUCCESS) {
        DeleteValue(kRestoreKey, t.tag);
        DeleteValue(kRestoreKey, marker.c_str());
    }
    return e;
}

// Removed only while it says 1, and recorded first.
const Tracked kDisabledByPolicy = {kSystemPolicyKey, L"DisableLogonBackgroundImage",
                                   L"System.DisableLogonBackgroundImage"};

std::wstring LockScreenKey(const std::wstring &sid) {
    return std::wstring(kProtectedKey) + L"\\" + sid + L"\\AnyoneRead\\LockScreen";
}

// The accounts Windows keeps the setting for: local and domain users, and SYSTEM's own,
// which the sign-in screen falls back to.
std::vector<std::wstring> ProtectedSids() {
    std::vector<std::wstring> sids;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProtectedKey, 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &h) !=
        ERROR_SUCCESS)
        return sids;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring sid(name, n);
        if (sid == L"S-1-5-18" || sid.rfind(L"S-1-5-21-", 0) == 0) sids.push_back(sid);
    }
    RegCloseKey(h);
    return sids;
}

// Restore-key values recording a sign-in background setting, by SID.
std::vector<std::wstring> RecordedSignInSids() {
    std::vector<std::wstring> sids;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRestoreKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return sids;
    const std::wstring prefix = kSignInTag, suffix = L".Recorded";
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumValueW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring v(name, n);
        if (v.size() > prefix.size() + suffix.size() && v.rfind(prefix, 0) == 0 &&
            v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0)
            sids.push_back(v.substr(prefix.size(), v.size() - prefix.size() - suffix.size()));
    }
    RegCloseKey(h);
    return sids;
}

class Service {
public:
    explicit Service(DWORD access) {
        scm_ = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm_) svc_ = OpenServiceW(scm_, paths::kServiceName, access);
        error_ = svc_ ? ERROR_SUCCESS : GetLastError();
    }
    ~Service() {
        if (svc_) CloseServiceHandle(svc_);
        if (scm_) CloseServiceHandle(scm_);
    }
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;
    SC_HANDLE get() const { return svc_; }
    DWORD error() const { return error_; }

private:
    SC_HANDLE scm_ = nullptr;
    SC_HANDLE svc_ = nullptr;
    DWORD error_ = ERROR_SUCCESS;
};

}  // namespace

DWORD ApplyWindowsLockScreen(const std::wstring &image) {
    // Nothing changes until the originals are safely recorded.
    DWORD e = ERROR_SUCCESS;
    for (const TrackedKey &k : kTrackedKeys)
        if (e == ERROR_SUCCESS) e = RememberKey(k);
    for (const Tracked &t : kTracked)
        if (e == ERROR_SUCCESS) e = Remember(t);
    if (e != ERROR_SUCCESS) return e;
    e = WriteDword(kPolicyKey, L"NoLockScreen", 1);
    if (e != ERROR_SUCCESS) return e;
    // The policy that paints a flat colour instead of the background goes, recorded.
    DWORD disabled = 0;
    if (AsDword(ReadValue(kDisabledByPolicy.key, kDisabledByPolicy.value), &disabled) && disabled) {
        e = Remember(kDisabledByPolicy);
        if (e == ERROR_SUCCESS) e = DeleteValue(kDisabledByPolicy.key, kDisabledByPolicy.value);
        if (e != ERROR_SUCCESS) return e;
    }
    // Every change to these values costs one black sign-in background, so an unchanged
    // value is left alone.
    DWORD status = 0;
    if (AsString(ReadValue(kCspKey, L"LockScreenImagePath")) != image) e = WriteString(kCspKey, L"LockScreenImagePath", image);
    if (e == ERROR_SUCCESS && AsString(ReadValue(kCspKey, L"LockScreenImageUrl")) != image)
        e = WriteString(kCspKey, L"LockScreenImageUrl", image);
    if (e == ERROR_SUCCESS && !(AsDword(ReadValue(kCspKey, L"LockScreenImageStatus"), &status) && status == 1))
        e = WriteDword(kCspKey, L"LockScreenImageStatus", 1);
    return e;
}

DWORD RestoreWindowsLockScreen() {
    DWORD first = Restore(kDisabledByPolicy);
    for (const Tracked &t : kTracked) {
        const DWORD e = Restore(t);
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    }
    // The keys only once every value in them is back.
    if (first != ERROR_SUCCESS) return first;
    for (const TrackedKey &k : kTrackedKeys) {
        const DWORD e = RestoreKey(k);
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    }
    return first;
}

bool WindowsLockScreenApplied() {
    DWORD v = 0;
    return AsDword(ReadValue(kPolicyKey, L"NoLockScreen"), &v) && v == 1;
}

bool LogonBackgroundDisabledByPolicy() {
    DWORD v = 0;
    return AsDword(ReadValue(kSystemPolicyKey, L"DisableLogonBackgroundImage"), &v) && v != 0;
}

DWORD ShowSignInBackground() {
    DWORD first = ERROR_SUCCESS;
    for (const std::wstring &sid : ProtectedSids()) {
        const std::wstring key = LockScreenKey(sid), tag = kSignInTag + sid;
        DWORD hidden = 0;
        if (!AsDword(ReadValue(key.c_str(), kHideValue), &hidden) || !hidden) continue;  // absent or 0: shown
        const Tracked t{key.c_str(), kHideValue, tag.c_str()};
        DWORD e = Remember(t);
        if (e == ERROR_SUCCESS) e = WriteDword(key.c_str(), kHideValue, 0);
        if (e == ERROR_SUCCESS) ALOG(L"sign-in background: turned on for %s", sid.c_str());
        else if (first == ERROR_SUCCESS) first = e;
    }
    return first;
}

DWORD RestoreSignInBackground() {
    DWORD first = ERROR_SUCCESS;
    for (const std::wstring &sid : RecordedSignInSids()) {
        const std::wstring key = LockScreenKey(sid), tag = kSignInTag + sid;
        const DWORD e = Restore(Tracked{key.c_str(), kHideValue, tag.c_str()});
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    }
    return first;
}

bool SignInBackgroundRestorePending() { return !RecordedSignInSids().empty(); }

DWORD AskServiceToRestore() {
    if (!SignInBackgroundRestorePending()) return ERROR_SUCCESS;
    Service s(SERVICE_USER_DEFINED_CONTROL | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!s.get()) return s.error();
    SERVICE_STATUS status{};
    if (QueryServiceStatus(s.get(), &status) && status.dwCurrentState == SERVICE_RUNNING) {
        if (!ControlService(s.get(), kServiceControlRestore, &status)) return GetLastError();
    } else {
        const wchar_t *args[] = {paths::kServiceName, kServiceRestoreArg};
        if (!StartServiceW(s.get(), 2, args)) return GetLastError();
        // It restores and stops by itself.
        for (int i = 0; i < 100; ++i) {
            if (QueryServiceStatus(s.get(), &status) && status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
    }
    return SignInBackgroundRestorePending() ? ERROR_CAN_NOT_COMPLETE : ERROR_SUCCESS;
}

DWORD InstallService(const std::wstring &launcherPath) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    const std::wstring command = L"\"" + launcherPath + L"\"";
    SC_HANDLE svc = OpenServiceW(scm, paths::kServiceName, SERVICE_ALL_ACCESS);
    DWORD e = ERROR_SUCCESS;
    if (svc) {
        if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, L"AnimeLogon"))
            e = GetLastError();
    } else {
        svc = CreateServiceW(scm, paths::kServiceName, L"AnimeLogon", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                             nullptr, nullptr);
        if (!svc) e = GetLastError();
    }
    if (svc) {
        SERVICE_DESCRIPTIONW description{const_cast<wchar_t *>(L"在登录界面上播放 AnimeLogon 视频。")};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description);
        // Restarted by the service manager if it ever stops unexpectedly.
        SC_ACTION actions[3] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
        SERVICE_FAILURE_ACTIONSW failure{};
        failure.dwResetPeriod = 24 * 3600;
        failure.cActions = ARRAYSIZE(actions);
        failure.lpsaActions = actions;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
        SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return e;
}

DWORD RemoveService() {
    StopLauncherService();
    Service s(DELETE);
    if (!s.get()) return s.error() == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : s.error();
    if (!DeleteService(s.get())) {
        const DWORD e = GetLastError();
        return e == ERROR_SERVICE_MARKED_FOR_DELETE ? ERROR_SUCCESS : e;
    }
    return ERROR_SUCCESS;
}

DWORD StartLauncherService() {
    Service s(SERVICE_START | SERVICE_QUERY_STATUS);
    if (!s.get()) return s.error();
    if (!::StartServiceW(s.get(), 0, nullptr)) {
        const DWORD e = GetLastError();
        return e == ERROR_SERVICE_ALREADY_RUNNING ? ERROR_SUCCESS : e;
    }
    return ERROR_SUCCESS;
}

DWORD StopLauncherService(DWORD waitMs) {
    Service s(SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s.get()) return s.error() == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : s.error();
    SERVICE_STATUS status{};
    if (!ControlService(s.get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD e = GetLastError();
        if (e != ERROR_SERVICE_NOT_ACTIVE) return e;
        return ERROR_SUCCESS;
    }
    const ULONGLONG until = GetTickCount64() + waitMs;
    while (GetTickCount64() < until) {
        if (!QueryServiceStatus(s.get(), &status)) return GetLastError();
        if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        Sleep(100);
    }
    return ERROR_TIMEOUT;
}

DWORD ServiceState() {
    Service s(SERVICE_QUERY_STATUS);
    if (!s.get()) return 0;
    SERVICE_STATUS status{};
    return QueryServiceStatus(s.get(), &status) ? status.dwCurrentState : 0;
}

DWORD SetServiceAutoStart(bool automatic) {
    Service s(SERVICE_CHANGE_CONFIG);
    if (!s.get()) return s.error();
    if (!ChangeServiceConfigW(s.get(), SERVICE_NO_CHANGE, automatic ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
                              SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
        return GetLastError();
    return ERROR_SUCCESS;
}

bool ServiceAutoStart() {
    Service s(SERVICE_QUERY_CONFIG);
    if (!s.get()) return false;
    DWORD need = 0;
    QueryServiceConfigW(s.get(), nullptr, 0, &need);
    std::vector<BYTE> buf(need ? need : sizeof(QUERY_SERVICE_CONFIGW));
    auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buf.data());
    if (!QueryServiceConfigW(s.get(), config, (DWORD)buf.size(), &need)) return false;
    return config->dwStartType == SERVICE_AUTO_START;
}

DWORD TurnOn(const std::wstring &launcherPath, const std::wstring &image) {
    // The service first: Windows' lock screen goes off only once something will cover it.
    DWORD e = ServiceState() == 0 ? InstallService(launcherPath) : ERROR_SUCCESS;
    if (e == ERROR_SUCCESS) e = SetServiceAutoStart(true);
    if (e == ERROR_SUCCESS) e = ApplyWindowsLockScreen(image);
    if (e == ERROR_SUCCESS) {
        ClearPausedMarker();
        e = StartLauncherService();
    }
    if (e != ERROR_SUCCESS) {
        ALOG(L"turn on: failed (%lu), undoing", e);
        TurnOff();
    }
    return e;
}

DWORD TurnOff() {
    const DWORD signIn = AskServiceToRestore();
    const DWORD stop = StopLauncherService();
    const DWORD start = SetServiceAutoStart(false);
    const DWORD restore = RestoreWindowsLockScreen();
    if (stop != ERROR_SUCCESS) return stop;
    if (start != ERROR_SUCCESS) return start;
    return restore != ERROR_SUCCESS ? restore : signIn;
}

bool IsOn() { return ServiceAutoStart() && WindowsLockScreenApplied(); }

bool Paused() { return GetFileAttributesW(paths::PausedMarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

bool WritePausedMarker(const wchar_t *reason) {
    HANDLE h = CreateFileW(paths::PausedMarkerPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::string text = ToUtf8(Format(
        L"AnimeLogon was paused on the logon screen on %04u-%02u-%02u %02u:%02u:%02u (%s).\r\n"
        L"Turn it back on from the AnimeLogon settings app.\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, reason));
    DWORD wrote = 0;
    ::WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
    return true;
}

bool ClearPausedMarker() {
    return DeleteFileW(paths::PausedMarkerPath().c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::wstring InstallDir() {
    return AsString(ReadValue(paths::kRegistryKey, L"InstallDir"));
}

DWORD WriteInstallRecord(const std::wstring &dir) {
    return WriteString(paths::kRegistryKey, L"InstallDir", dir);
}

DWORD RemoveInstallRecord(bool keepRestore) {
    if (!keepRestore) {
        const LSTATUS e = RegDeleteTreeW(HKEY_LOCAL_MACHINE, paths::kRegistryKey);
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : (DWORD)e;
    }
    HKEY h = nullptr;
    LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, paths::kRegistryKey, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS | DELETE | KEY_WOW64_64KEY, &h);
    if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (e != ERROR_SUCCESS) return (DWORD)e;
    // Names first, then deletions, so the enumeration is not disturbed.
    std::vector<std::wstring> keys, values;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (_wcsicmp(name, L"Restore") != 0) keys.push_back(name);
    }
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(name);
        if (RegEnumValueW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        values.push_back(name);
    }
    DWORD first = ERROR_SUCCESS;
    for (const std::wstring &k : keys) {
        e = RegDeleteTreeW(h, k.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND && first == ERROR_SUCCESS) first = (DWORD)e;
    }
    for (const std::wstring &v : values) {
        e = RegDeleteValueW(h, v.c_str());
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND && first == ERROR_SUCCESS) first = (DWORD)e;
    }
    RegCloseKey(h);
    return first;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

}  // namespace animelogon::machine
