#include "animelogon/others.h"

#include <windows.h>
#include <shlobj.h>
#include <taskschd.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>

#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/text.h"

using Microsoft::WRL::ComPtr;

namespace animelogon::others {
namespace {

// Shittim Logon, and builds made from its source, which install under the same names. Switched
// off here exactly as its own settings app switches it off: the marker its programs stand down
// for, its scheduled task disabled, its two processes stopped, its face-sign-in filter removed
// and the Windows values it set put back. Switching it on again from its settings undoes all of it.
constexpr const wchar_t *kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ShittimLogon";
constexpr const wchar_t *kFolder = L"ShittimLogon";  // under Program Files and ProgramData
constexpr const wchar_t *kTask = L"ShittimLogon";    // in the Task Scheduler's root folder
constexpr const wchar_t *kDefaultName = L"Shittim Logon";
constexpr const wchar_t *kFilterParent = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Provider Filters";
constexpr const wchar_t *kFilter = L"{7A4B2E10-9C3D-4F6A-B1E8-5D2C7F0A9B34}";
constexpr const wchar_t *kFaceGateKey = L"SOFTWARE\\ShittimLogon\\HelloGate";

constexpr const wchar_t *kCspKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP";
constexpr const wchar_t *kPersonalizationPolicyKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization";
constexpr const wchar_t *kSystemPolicyKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\System";
constexpr const wchar_t *kSignOnPolicyKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";

std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t *fallback) {
    wchar_t *base = nullptr;
    std::wstring dir = SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base)) && base ? base : fallback;
    CoTaskMemFree(base);
    return dir;
}

std::wstring ReadString(const wchar_t *key, const wchar_t *name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

bool KeyExists(const wchar_t *key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
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

// One spelling of a directory or file: separators collapsed, the long form of short names, no
// trailing backslash. Installers have recorded "D:\\Dir" for what Windows reports as "D:\Dir".
std::wstring Canonical(const std::wstring &path) {
    std::wstring out;
    for (wchar_t c : path) {
        if (c == L'/') c = L'\\';
        if (c == L'\\' && out.size() > 1 && out.back() == L'\\') continue;
        out += c;
    }
    wchar_t buf[2048];
    const DWORD n = GetLongPathNameW(out.c_str(), buf, ARRAYSIZE(buf));
    if (n && n < ARRAYSIZE(buf)) out.assign(buf, n);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool SamePath(const std::wstring &a, const std::wstring &b) {
    return !a.empty() && !b.empty() &&
           CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// `path` is `dir` or lies beneath it; both canonical.
bool Within(const std::wstring &path, const std::wstring &dir) {
    if (dir.empty() || path.size() < dir.size()) return false;
    if (CompareStringOrdinal(path.c_str(), (int)dir.size(), dir.c_str(), (int)dir.size(), TRUE) != CSTR_EQUAL) return false;
    return path.size() == dir.size() || dir.back() == L'\\' || path[dir.size()] == L'\\';
}

bool ShittimInstalled() { return KeyExists(kUninstallKey); }

std::wstring ProgramDir() {
    const std::wstring recorded = ReadString(kUninstallKey, L"InstallLocation");
    return Canonical(recorded.empty() ? KnownFolder(FOLDERID_ProgramFiles, L"C:\\Program Files") + L"\\" + kFolder
                                      : recorded);
}

std::wstring DataDir() { return KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData") + L"\\" + kFolder; }
std::wstring MarkerPath() { return DataDir() + L"\\disabled"; }
bool MarkerPresent() { return GetFileAttributesW(MarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring Name() {
    std::wstring name = ReadString(kUninstallKey, L"DisplayName");
    if (name.empty() || name.size() > 80) name = kDefaultName;
    return name;
}

// The file whose presence means "switched off" to every part of it. Its text is for whoever finds it.
DWORD WriteMarker() {
    if (MarkerPresent()) return ERROR_SUCCESS;
    CreateDirectoryW(DataDir().c_str(), nullptr);
    HANDLE h = CreateFileW(MarkerPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::string text = ToUtf8(Format(
        L"Shittim Logon was switched off here by AnimeLogon on %04u-%02u-%02u %02u:%02u:%02u.\r\n"
        L"Only one program can draw the logon screen at a time.\r\n"
        L"\r\n"
        L"To use Shittim Logon again, turn off \"Enable AnimeLogon\" in the AnimeLogon settings app,\r\n"
        L"then switch Shittim Logon back on in the Shittim Logon settings app.\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return e;
}

// Disabled rather than deleted, so its own switch can enable it again; then its running instance
// stopped. A disabled task is not restarted when its process is ended.
HRESULT DisableTask() {
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(hr = service->Connect(none, none, none, none))) return hr;
    ComPtr<ITaskFolder> root;
    BSTR rootName = SysAllocString(L"\\");
    hr = service->GetFolder(rootName, &root);
    SysFreeString(rootName);
    if (FAILED(hr)) return hr;
    ComPtr<IRegisteredTask> task;
    BSTR taskName = SysAllocString(kTask);
    hr = root->GetTask(taskName, &task);
    SysFreeString(taskName);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = task->put_Enabled(VARIANT_FALSE))) return hr;
    task->Stop(0);
    return S_OK;
}

// Its processes run as SYSTEM; an elevated administrator opens them with this privilege.
void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);
}

std::wstring ImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<wchar_t> buf(32768);
    DWORD size = (DWORD)buf.size();
    const bool ok = QueryFullProcessImageNameW(process, 0, buf.data(), &size);
    CloseHandle(process);
    return ok ? std::wstring(buf.data(), size) : std::wstring();
}

// Its supervisor (launcher.exe) and the overlay it starts, wherever they run directly from `dir`:
// the same names as AnimeLogon's own, so only the directory tells them apart. The supervisor goes
// first so that nothing starts another overlay. Returns how many would not stop.
int StopPrograms(const std::wstring &dir) {
    if (dir.empty() || SamePath(dir, Canonical(machine::InstallDir()))) return 0;
    EnableDebugPrivilege();
    std::vector<DWORD> tried;
    int failed = 0;
    auto stop = [&](const wchar_t *name) {
        std::vector<DWORD> pids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, name) != 0 || std::find(tried.begin(), tried.end(), pe.th32ProcessID) != tried.end())
                continue;
            const std::wstring image = ImagePath(pe.th32ProcessID);
            const size_t slash = image.find_last_of(L'\\');
            if (slash != std::wstring::npos && SamePath(Canonical(image.substr(0, slash)), dir)) pids.push_back(pe.th32ProcessID);
        }
        CloseHandle(snap);
        for (DWORD pid : pids) {
            tried.push_back(pid);
            // Stopping its task may already have ended it: then terminating fails, and it is gone.
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            const bool vanished = !h && GetLastError() == ERROR_INVALID_PARAMETER;
            if (h) TerminateProcess(h, 0);
            const bool gone = vanished || (h && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
            if (h) CloseHandle(h);
            ALOG(L"others: %s %s (%lu)", gone ? L"stopped" : L"could not stop", name, pid);
            if (!gone) ++failed;
        }
        return (int)pids.size();
    };
    stop(L"launcher.exe");
    stop(L"overlay.exe");
    // An overlay the supervisor started just before it went.
    for (int pass = 0; pass < 3; ++pass) {
        Sleep(200);
        if (!stop(L"overlay.exe")) break;
    }
    return failed;
}

// Its optional face sign-in delay is a credential provider filter: left enrolled with nothing to
// end it, it would hold face sign-in back on every logon. Its own setting goes off with it.
void RemoveFaceFilter() {
    HKEY parent = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kFilterParent, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &parent) == ERROR_SUCCESS) {
        const LSTATUS e = RegDeleteTreeW(parent, kFilter);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) ALOG(L"others: cannot remove the face filter (%ld)", e);
        RegCloseKey(parent);
    }
    HKEY gate = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kFaceGateKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &gate) == ERROR_SUCCESS) {
        const DWORD zero = 0;
        RegSetValueExW(gate, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
        RegCloseKey(gate);
    }
}

std::wstring CspImage() { return Canonical(ReadString(kCspKey, L"LockScreenImagePath")); }

// What it set for Windows to draw, deleted as its own switch deletes it: the sign-in background,
// the lock screen policy, the unblurred sign-in background and the restart sign-on policy. The
// background stays when it is AnimeLogon's already, since every change of it costs one black
// sign-in screen.
void RestoreWindowsValues(const std::wstring &ownImage) {
    if (!SamePath(CspImage(), Canonical(ownImage)))
        for (const wchar_t *v : {L"LockScreenImagePath", L"LockScreenImageUrl", L"LockScreenImageStatus"})
            DeleteValue(kCspKey, v);
    DeleteValue(kPersonalizationPolicyKey, L"NoLockScreen");
    DeleteValue(kSystemPolicyKey, L"DisableAcrylicBackgroundOnLogon");
    DeleteValue(kSignOnPolicyKey, L"DisableAutomaticRestartSignOn");
}

}  // namespace

std::vector<Program> Installed() {
    std::vector<Program> found;
    if (ShittimInstalled()) found.push_back({Name(), !MarkerPresent()});
    return found;
}

bool AnyOn() {
    for (const Program &p : Installed())
        if (p.on) return true;
    return false;
}

DWORD TurnOff(const std::wstring &ownImage) {
    if (!ShittimInstalled()) return ERROR_SUCCESS;
    const std::wstring dir = ProgramDir();
    const bool on = !MarkerPresent();
    // Switched off from its logon screen, it stops drawing but leaves Windows pointing at its picture.
    const std::wstring image = CspImage();
    const bool leftBehind = Within(image, dir) || Within(image, Canonical(DataDir()));
    ALOG(L"others: switching off Shittim Logon in %s (%s)", dir.c_str(), on ? L"on" : L"off already");

    // The marker first: whatever of it still runs stands down when it appears.
    const DWORD marker = WriteMarker();
    if (marker != ERROR_SUCCESS) {
        ALOG(L"others: cannot write %s (%lu)", MarkerPath().c_str(), marker);
        return marker;
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT task = DisableTask();
    if (SUCCEEDED(co)) CoUninitialize();
    if (FAILED(task)) ALOG(L"others: cannot disable its task (0x%08lx)", (unsigned long)task);
    StopPrograms(dir);
    RemoveFaceFilter();
    if (on || leftBehind) RestoreWindowsValues(ownImage);
    return ERROR_SUCCESS;
}

}  // namespace animelogon::others
