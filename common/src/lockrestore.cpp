#include "animelogon/lockrestore.h"

#include <windows.h>

#include <vector>

#include "animelogon/log.h"
#include "animelogon/text.h"

namespace animelogon::lockrestore {
namespace {

constexpr const wchar_t *kProfileList = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
constexpr const wchar_t *kRecordKey = L"Software\\AnimeLogon\\LockScreen";
constexpr const wchar_t *kRunOnceKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr const wchar_t *kRunOnceValue = L"AnimeLogon.RestoreLockScreen";
constexpr const wchar_t *kScriptName = L"restore-lock-screen.ps1";

// Runs as the account, once, at its sign-in. Windows deletes the RunOnce value before running it.
constexpr const char kScript[] = R"PS(# AnimeLogon was removed from this computer. This puts back the lock screen picture this account
# had before AnimeLogon set one, and Windows Spotlight if it was on, then deletes itself. It runs once,
# at sign-in, from this account's RunOnce key.
$ErrorActionPreference = 'SilentlyContinue'
$record = 'HKCU:\Software\AnimeLogon\LockScreen'
$r = Get-ItemProperty -Path $record
# Installed again since: the record stays for that install.
if ((-not (Test-Path 'HKLM:\SOFTWARE\AnimeLogon')) -and $r -and $r.Recorded -eq 1) {
    $from = $null
    if ($r.File -and ($r.File -notmatch '\\AnimeLogon\\signin\\') -and (Test-Path -LiteralPath $r.File)) { $from = $r.File }
    elseif ($r.Copy -and (Test-Path -LiteralPath $r.Copy)) { $from = $r.Copy }
    $done = $true
    if ($from) {
        $done = $false
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $ext = [System.WindowsRuntimeSystemExtensions].GetMethods()
        $op = ($ext | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
        $act = ($ext | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]
        [void][Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
        [void][Windows.System.UserProfile.LockScreen, Windows.System.UserProfile, ContentType = WindowsRuntime]
        # Right after sign-in the lock screen settings may not answer yet.
        for ($i = 0; $i -lt 6 -and -not $done; $i++) {
            try {
                $t = $op.MakeGenericMethod([Windows.Storage.StorageFile]).Invoke($null, @([Windows.Storage.StorageFile]::GetFileFromPathAsync($from)))
                $t.Wait(-1) | Out-Null
                $a = $act.Invoke($null, @([Windows.System.UserProfile.LockScreen]::SetImageFileAsync($t.Result)))
                $a.Wait(-1) | Out-Null
                $done = $true
            } catch { Start-Sleep -Seconds 5 }
        }
    }
    if ($done) {
        # After the picture: setting it turned Spotlight off.
        if ($null -ne $r.Spotlight) {
            New-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager' -Name 'RotatingLockScreenEnabled' -Value ([int]$r.Spotlight) -PropertyType DWord -Force | Out-Null
        }
        if ($r.Copy) { Remove-Item -LiteralPath $r.Copy -Force }
        Remove-Item -Path $record -Recurse -Force
        $left = Get-Item -Path 'HKCU:\Software\AnimeLogon'
        if ($left -and $left.SubKeyCount -eq 0 -and $left.ValueCount -eq 0) { Remove-Item -Path 'HKCU:\Software\AnimeLogon' -Force }
    }
}
Remove-Item -LiteralPath $PSCommandPath -Force
)PS";

bool EnablePrivilege(const wchar_t *name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

std::wstring ReadString(HKEY root, const wchar_t *key, const wchar_t *name) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

// The account's settings: HKU\<sid> while it is signed in, else its NTUSER.DAT loaded under a name
// of ours, unloaded again when this goes.
class Hive {
public:
    Hive(const std::wstring &sid, const std::wstring &profile) {
        if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ | KEY_WRITE, &root_) == ERROR_SUCCESS) return;
        root_ = nullptr;
        loaded_ = L"AnimeLogon." + sid;
        error_ = (DWORD)RegLoadKeyW(HKEY_USERS, loaded_.c_str(), (profile + L"\\NTUSER.DAT").c_str());
        if (error_ != ERROR_SUCCESS) {
            loaded_.clear();
            return;
        }
        error_ = (DWORD)RegOpenKeyExW(HKEY_USERS, loaded_.c_str(), 0, KEY_READ | KEY_WRITE, &root_);
        if (error_ != ERROR_SUCCESS) root_ = nullptr;
    }
    ~Hive() {
        if (root_) RegCloseKey(root_);
        if (!loaded_.empty()) RegUnLoadKeyW(HKEY_USERS, loaded_.c_str());
    }
    Hive(const Hive &) = delete;
    Hive &operator=(const Hive &) = delete;
    HKEY root() const { return root_; }
    DWORD error() const { return error_; }

private:
    HKEY root_ = nullptr;
    std::wstring loaded_;
    DWORD error_ = ERROR_SUCCESS;
};

bool WriteScript(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const DWORD size = (DWORD)(sizeof(kScript) - 1);
    const bool ok = WriteFile(h, kScript, size, &wrote, nullptr) && wrote == size;
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

}  // namespace

const char *Script() { return kScript; }

std::wstring CommandFor(const std::wstring &scriptPath) {
    wchar_t system[MAX_PATH];
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring dir = n && n < MAX_PATH ? std::wstring(system, n) : std::wstring(L"C:\\Windows\\System32");
    return L"\"" + dir + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -WindowStyle Hidden "
           L"-ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";
}

int ScheduleForAccounts(int *unreachable) {
    *unreachable = 0;
    // Loading the settings of an account that is not signed in needs both.
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);
    HKEY list = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProfileList, 0, KEY_READ | KEY_WOW64_64KEY, &list) != ERROR_SUCCESS) return 0;
    int scheduled = 0;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(list, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring sid(name, length);
        if (sid.rfind(L"S-1-5-21-", 0) != 0) continue;  // people's accounts only
        std::wstring profile = ReadString(list, sid.c_str(), L"ProfileImagePath");
        wchar_t expanded[MAX_PATH * 2];
        if (!profile.empty() && ExpandEnvironmentStringsW(profile.c_str(), expanded, ARRAYSIZE(expanded))) profile = expanded;
        if (profile.empty() || GetFileAttributesW(profile.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        Hive hive(sid, profile);
        if (!hive.root()) {
            ALOG(L"lock screen: the settings of %s cannot be read (%lu)", sid.c_str(), hive.error());
            ++*unreachable;
            continue;
        }
        DWORD recorded = 0, size = sizeof(recorded);
        if (RegGetValueW(hive.root(), kRecordKey, L"Recorded", RRF_RT_REG_DWORD, nullptr, &recorded, &size) != ERROR_SUCCESS ||
            !recorded)
            continue;  // nothing of AnimeLogon's set, or already put back
        const std::wstring dir = profile + L"\\AppData\\Local\\AnimeLogon", script = dir + L"\\" + kScriptName;
        CreateDirectoryW((profile + L"\\AppData\\Local").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring command = CommandFor(script);
        HKEY run = nullptr;
        LSTATUS e = WriteScript(script) ? ERROR_SUCCESS : (LSTATUS)GetLastError();
        if (e == ERROR_SUCCESS)
            e = RegCreateKeyExW(hive.root(), kRunOnceKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &run, nullptr);
        if (e == ERROR_SUCCESS) {
            e = RegSetValueExW(run, kRunOnceValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                               (DWORD)((command.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(run);
        }
        if (e == ERROR_SUCCESS) {
            ++scheduled;
            ALOG(L"lock screen: %s gets its own picture back at its next sign-in", sid.c_str());
        } else {
            ++*unreachable;
            ALOG(L"lock screen: %s cannot be given its picture back (%ld)", sid.c_str(), e);
        }
    }
    RegCloseKey(list);
    return scheduled;
}

}  // namespace animelogon::lockrestore
