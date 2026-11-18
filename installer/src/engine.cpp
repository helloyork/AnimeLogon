#include "engine.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <aclapi.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>

#include "animelogon/background.h"
#include "animelogon/components.h"
#include "animelogon/i18n.h"
#include "animelogon/live.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/others.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/sha256.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/version.h"
#include "animelogon/wallpaper.h"

#include "assoc.h"
#include "payload.h"
#include "restart.h"

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace engine {

namespace {

using i18n::Tr;

// What is installed is listed once, in installer/CMakeLists.txt (payload.h): the programs --
// WebView2Loader.dll is the settings app's, which finds the Edge WebView2 runtime its page runs in --
// and the licences of AnimeLogon and of the code it carries, beside them. Only install.exe as built
// carries the licences; a build run from its own folder installs the programs beside it, without.

DWORD SceneReadAccess(const std::wstring &path, bool directory) {
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(sid);
    if (!CreateWellKnownSid(WinBuiltinAnyPackageSid,nullptr,sid,&size)) return GetLastError();
    PACL oldAcl = nullptr, newAcl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD e = GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&oldAcl,nullptr,&sd);
    if (e != ERROR_SUCCESS) return e;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = directory ? FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL : FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS; access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    e = SetEntriesInAclW(1,&access,oldAcl,&newAcl);
    if (e == ERROR_SUCCESS) e = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,newAcl,nullptr);
    if (newAcl) LocalFree(newAcl); LocalFree(sd); return e;
}

// Installed by earlier versions and no longer: config.exe was the settings app before
// animelogon.exe. Deleted by install and by uninstall.
const wchar_t *const kRetired[] = {L"config.exe"};

constexpr const wchar_t *kDisplayName = L"AnimeLogon";
constexpr const wchar_t *kUninstallKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AnimeLogon";
// The app is the theme library as well as the settings, so its shortcut carries its own name, the
// same in every language. Version 0.1.0 named it after the settings.
constexpr const wchar_t *kShortcut = L"AnimeLogon.lnk";
constexpr const wchar_t *kOldShortcut = L"AnimeLogon \u8bbe\u7f6e.lnk";
constexpr const wchar_t *kProjectUrl = L"https://github.com/helloyork/AnimeLogon";
// Windows starts the icon in the notification area at every sign-in, for every account, from here;
// it ends at once where settings.ini's `tray` is off.
constexpr const wchar_t *kRunKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t *kRunValue = L"AnimeLogon";
// How long the settings app and the icon are given to close before their file is replaced.
constexpr DWORD kCloseWaitMs = 5000;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t *base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base)) && base) dir = base;
    CoTaskMemFree(base);
    return dir;
}

std::wstring SourceDir() { return paths::ModuleDir(); }
std::wstring LauncherPath(const std::wstring &dir) { return dir + L"\\launcher.exe"; }
std::wstring AppPath(const std::wstring &dir) { return dir + L"\\animelogon.exe"; }

std::wstring Shortcut(REFKNOWNFOLDERID folder, const wchar_t *name) {
    const std::wstring dir = KnownFolder(folder);
    return dir.empty() ? std::wstring() : dir + L"\\" + name;
}

std::wstring WithError(const std::wstring &text, DWORD e) {
    return Tr("error.with_code", {{L"text", text}, {L"code", std::to_wstring(e)}});
}

DWORD WriteRegString(HKEY key, const wchar_t *name, const std::wstring &value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

DWORD WriteRegDword(HKEY key, const wchar_t *name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
}

HRESULT CreateShortcut(const std::wstring &linkPath, const std::wstring &target, const std::wstring &icon) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return hr;
    link->SetPath(target.c_str());
    link->SetIconLocation(icon.c_str(), 0);
    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    return SUCCEEDED(hr) ? file->Save(linkPath.c_str(), TRUE) : hr;
}

DWORD WriteRunEntry(const std::wstring &targetDir) {
    HKEY key = nullptr;
    DWORD e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                              nullptr);
    if (e != ERROR_SUCCESS) return e;
    e = WriteRegString(key, kRunValue, L"\"" + AppPath(targetDir) + L"\" --tray");
    RegCloseKey(key);
    return e;
}

DWORD RemoveRunEntry() {
    HKEY key = nullptr;
    DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (e == ERROR_SUCCESS) {
        e = RegDeleteValueW(key, kRunValue);
        RegCloseKey(key);
    }
    return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
}

// The settings app and the icon in the notification area run animelogon.exe, which cannot be
// replaced or deleted while they do. Those on this desktop are asked to close, as at sign-out,
// and waited for; a sandbox's are not AnimeLogon's own (their title names the sandbox, and they
// run another copy). Another account's, signed in at the same time, are out of reach
// (WriteOver moves their file aside). Returns whether the icon was among them.
bool CloseRunningApp() {
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            wchar_t name[32] = {}, title[32] = {};
            GetClassNameW(hwnd, name, ARRAYSIZE(name));
            GetWindowTextW(hwnd, title, ARRAYSIZE(title));
            if ((wcscmp(name, L"AnimeLogonApp") == 0 || wcscmp(name, L"AnimeLogonTray") == 0) && wcscmp(title, kDisplayName) == 0)
                reinterpret_cast<std::vector<HWND> *>(lp)->push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    bool tray = false;
    std::vector<HANDLE> processes;
    for (HWND w : windows) {
        wchar_t name[32] = {};
        GetClassNameW(w, name, ARRAYSIZE(name));
        tray = tray || wcscmp(name, L"AnimeLogonTray") == 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) processes.push_back(p);
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
    if (!processes.empty() &&
        WaitForMultipleObjects((DWORD)processes.size(), processes.data(), TRUE, kCloseWaitMs) == WAIT_TIMEOUT)
        ALOG(L"AnimeLogon's windows did not all close");
    else if (!windows.empty())
        ALOG(L"closed %zu of AnimeLogon's windows", windows.size());
    for (HANDLE p : processes) CloseHandle(p);
    return tray;
}

// Starts the icon in the notification area as the person whose desktop this is, not elevated as
// the installer is: through Explorer's own ShellExecute, the way the shell starts what is
// double-clicked.
// Nothing is started where this desktop has no shell (a scripted install as SYSTEM, a service's
// session): asking COM for the shell's windows there would start a server just to find none.
HRESULT StartTrayOnDesktop(const std::wstring &targetDir) {
    if (!GetShellWindow()) return S_FALSE;
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return hr;
    VARIANT desktop{}, empty{};
    desktop.vt = VT_I4;
    desktop.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> found;
    hr = windows->FindWindowSW(&desktop, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &found);
    if (hr != S_OK || !found) return FAILED(hr) ? hr : E_FAIL;  // S_FALSE: no desktop window
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background, application;
    ComPtr<IShellFolderViewDual> folder;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(hr = found.As(&provider)) || FAILED(hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(hr = browser->QueryActiveShellView(&view)) ||
        FAILED(hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(hr = background.As(&folder)) ||
        FAILED(hr = folder->get_Application(&application)) || FAILED(hr = application.As(&shell)))
        return hr;
    BSTR file = SysAllocString(AppPath(targetDir).c_str());
    VARIANT args{}, dir{}, verb{}, show{};
    args.vt = dir.vt = verb.vt = VT_BSTR;
    args.bstrVal = SysAllocString(L"--tray");
    dir.bstrVal = SysAllocString(targetDir.c_str());
    verb.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    hr = shell->ShellExecute(file, args, dir, verb, show);
    SysFreeString(file);
    VariantClear(&args);
    VariantClear(&dir);
    VariantClear(&verb);
    return hr;
}

// A quick directory size, for the uninstall entry's EstimatedSize.
DWORD DirSizeKb(const std::wstring &dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return (DWORD)(total / 1024);
}

DWORD WriteUninstallEntry(const std::wstring &targetDir) {
    HKEY key = nullptr;
    DWORD e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                              nullptr, &key, nullptr);
    if (e != ERROR_SUCCESS) return e;
    const std::wstring uninstall = L"\"" + targetDir + L"\\uninstall.exe\"";
    const DWORD results[] = {
        WriteRegString(key, L"DisplayName", kDisplayName),
        WriteRegString(key, L"DisplayVersion", FromUtf8(ANIMELOGON_VERSION)),
        WriteRegString(key, L"Publisher", FromUtf8(ANIMELOGON_COMPANY)),
        WriteRegString(key, L"URLInfoAbout", kProjectUrl),
        WriteRegString(key, L"HelpLink", std::wstring(kProjectUrl) + L"/issues"),
        WriteRegString(key, L"InstallLocation", targetDir),
        WriteRegString(key, L"DisplayIcon", AppPath(targetDir)),
        WriteRegString(key, L"UninstallString", uninstall),
        WriteRegString(key, L"QuietUninstallString", uninstall + L" --quiet"),
        WriteRegDword(key, L"NoModify", 1),
        WriteRegDword(key, L"NoRepair", 1),
        WriteRegDword(key, L"EstimatedSize", DirSizeKb(targetDir)),
    };
    RegCloseKey(key);
    for (DWORD r : results)
        if (r != ERROR_SUCCESS) return r;
    return ERROR_SUCCESS;
}

// %ProgramData%\AnimeLogon and the directories in it, taken back and emptied if anyone but
// an administrator could have prepared them: the logs, and the stores themes are made of.
DWORD PrepareDataDir() {
    for (const std::wstring &dir : {paths::DataDir(), paths::LogDir(), WallpapersDir(), ComponentsDir(), ThemesDir()}) {
        const DWORD e = secure::SecureDirectory(dir);
        if (e != ERROR_SUCCESS) return e;
    }
    return ERROR_SUCCESS;
}

// The video install.exe carries for the bundled wallpaper (installer/CMakeLists.txt).
constexpr const wchar_t *kBundledVideo = L"soft-hills.mp4";

// The bundled wallpaper, the default theme's, written into the wallpaper store from install.exe
// itself and the way the settings app's elevated helper writes an imported one: administrators-
// only, wallpaper.ini written afresh, the sha256 computed from the video. An entry already there
// with the same video is kept -- the overlay may be showing it -- and anything else under the id
// is replaced.
DWORD PlaceBundledWallpaper() {
    const void *data = nullptr;
    DWORD size = 0;
    if (!payload::Bytes(kBundledVideo, 0, &data, &size)) return ERROR_FILE_NOT_FOUND;
    const std::wstring sha256 = Sha256Of(data, size);
    if (sha256.empty()) return ERROR_INVALID_DATA;
    WallpaperInfo have;
    if (LoadWallpaper(kBundledWallpaper, &have) && have.kind == WallpaperKind::Video && have.sha256 == sha256)
        return ERROR_SUCCESS;
    const std::wstring dir = WallpaperDir(kBundledWallpaper);
    DWORD e = secure::RemoveTree(dir);
    if (e == ERROR_SUCCESS) e = secure::SecureDirectory(dir);
    if (e == ERROR_SUCCESS) e = secure::WriteBytes(WallpaperVideoPath(kBundledWallpaper), data, size);
    if (e == ERROR_SUCCESS) {
        const std::string ini = ToUtf8(SerializeWallpaperInfo(BundledWallpaperInfo(sha256)));
        e = secure::WriteBytes(WallpaperInfoPath(kBundledWallpaper), ini.data(), ini.size());
    }
    if (e != ERROR_SUCCESS) secure::RemoveTree(dir);
    return e;
}

// The stores from before themes, videos\ and skins\. Nothing reads them, and nothing in them
// carries over: themes replaced them without a migration.
void RemoveOldStores() {
    for (const std::wstring &dir : {paths::DataDir() + L"\\videos", paths::DataDir() + L"\\skins"}) {
        const DWORD e = secure::RemoveTree(dir);
        if (e != ERROR_SUCCESS) ALOG(L"%s cannot be removed (%lu)", dir.c_str(), e);
    }
}

// Whether settings.ini names a theme. One from before themes does not, and its keys mean
// nothing to this version.
bool SettingsNameATheme() {
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(paths::SettingsPath(), &bytes, kMaxSettingsBytes)) return false;
    const size_t skip = (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
    return NamesATheme(FromUtf8(std::string_view((const char *)bytes.data() + skip, bytes.size() - skip)));
}

// Deletes an installed file now, or queues it for the next restart if it is in use.
DWORD RemoveFileOrQueue(const std::wstring &path) {
    if (DeleteFileW(path.c_str())) return ERROR_SUCCESS;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        ALOG(L"%s is in use (%lu); deleted at the next restart", path.c_str(), e);
        return ERROR_SUCCESS;
    }
    e = GetLastError();
    ALOG(L"cannot delete %s (%lu)", path.c_str(), e);
    return e;
}

// Whether the settings app will find the Edge WebView2 runtime, asked of the loader it ships
// with, the copy just installed. Windows 11 has the runtime built in; Windows 10 may not.
// The ARM64 loader cannot be loaded into this x64 program; for ARM64 (Windows 11 only), the
// runtime's own registration answers instead.
bool WebView2RuntimePresent(const std::wstring &targetDir, USHORT arch) {
    if (arch != payload::OwnMachine()) return WebView2Installed();
    HMODULE loader = LoadLibraryExW((targetDir + L"\\WebView2Loader.dll").c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) return false;
    using Find = HRESULT(STDAPICALLTYPE *)(PCWSTR, LPWSTR *);
    const auto find = reinterpret_cast<Find>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
    LPWSTR version = nullptr;
    const bool present = find && SUCCEEDED(find(nullptr, &version)) && version;
    if (present) ALOG(L"install: WebView2 runtime %s", version);
    CoTaskMemFree(version);
    FreeLibrary(loader);
    return present;
}

// The nearest directory of `path` that exists, for asking about its volume.
std::wstring ExistingAncestor(std::wstring path) {
    while (!path.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return L"";
        path.resize(slash);
        if (path.size() == 2 && path[1] == L':') path += L'\\';
    }
    return path;
}

// Writes an installed file with `write`. One still running -- the settings app or the tray icon in
// another account's session, which CloseRunningApp cannot reach -- is moved aside first (a running
// program's file can be renamed, not replaced) and deleted at the next restart; it goes on running
// until it ends, and the next start is the new one.
DWORD WriteOver(const std::wstring &to, const std::function<DWORD()> &write) {
    const DWORD e = write();
    if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) return e;
    const std::wstring aside = to + L"." + std::to_wstring(GetTickCount64()) + L".old";
    if (!MoveFileExW(to.c_str(), aside.c_str(), 0)) return e;
    const DWORD again = write();
    if (again != ERROR_SUCCESS) {
        MoveFileExW(aside.c_str(), to.c_str(), 0);
        return again;
    }
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    ALOG(L"%s was in use: the old copy goes at the next restart", to.c_str());
    return ERROR_SUCCESS;
}

// Writes one program (the one for `arch`) or document into the install directory: from inside install.exe,
// or from beside it when this build carries none. A copy loses the mark of a download
// (Zone.Identifier) that CopyFileW carries over from files unpacked from a downloaded zip.
DWORD Place(const wchar_t *name, USHORT arch, const std::wstring &to, bool required) {
    const bool carried = payload::Has(name, arch);
    const std::wstring from = SourceDir() + L"\\" + name;
    if (!carried && GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) return required ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
    return WriteOver(to, [&]() -> DWORD {
        if (carried) return payload::Write(name, arch, to);
        if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) return GetLastError();
        DeleteFileW((to + L":Zone.Identifier").c_str());
        return ERROR_SUCCESS;
    });
}

}  // namespace

std::wstring ProgramFilesDir() {
    const std::wstring dir = KnownFolder(FOLDERID_ProgramFiles);
    return dir.empty() ? std::wstring(L"C:\\Program Files") : dir;
}

// The SYSTEM service runs from here, so every directory on the way must already be closed
// to everyone but administrators. Program Files is; an arbitrary --dir may not be.
bool InstallDirAllowed(const std::wstring &dir) {
    const std::wstring root = ProgramFilesDir();
    return paths::IsPlainAbsolute(dir) && paths::IsWithin(dir, root) && dir.size() > root.size();
}

bool WebView2Installed() {
    // Microsoft's documented check: the runtime's EdgeUpdate client key has a version in "pv",
    // per machine (under the 32-bit view) or per user.
    const std::wstring client = L"Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const std::pair<HKEY, std::wstring> places[] = {{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\" + client},
                                                    {HKEY_CURRENT_USER, L"Software\\" + client}};
    for (const auto &[root, key] : places) {
        wchar_t version[64] = {};
        DWORD size = sizeof(version) - sizeof(wchar_t);
        if (RegGetValueW(root, key.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, version, &size) == ERROR_SUCCESS && version[0] &&
            wcscmp(version, L"0.0.0.0") != 0)
            return true;
    }
    return false;
}

std::wstring InstalledVersion() {
    if (machine::InstallDir().empty()) return L"";
    wchar_t version[64] = {};
    DWORD size = sizeof(version) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                     version, &size) != ERROR_SUCCESS)
        return L"?";
    return version;
}

unsigned long long FreeBytes(const std::wstring &dir) {
    const std::wstring existing = ExistingAncestor(dir);
    ULARGE_INTEGER free{};
    if (existing.empty() || !GetDiskFreeSpaceExW(existing.c_str(), &free, nullptr, nullptr)) return 0;
    return free.QuadPart;
}

unsigned long long RequiredBytes() {
    // The programs and licences, and room for the logs and the sign-in background. A build without
    // a payload counts the files beside it.
    unsigned long long total = payload::TotalBytes(payload::InstallMachine());
    if (!total)
        for (const wchar_t *c : payload::Names(payload::Kind::Program)) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW((SourceDir() + L"\\" + c).c_str(), GetFileExInfoStandard, &a))
                total += ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        }
    return total + 1024 * 1024;
}

std::vector<Blocker> CheckBeforeInstall(const std::wstring &targetDir) {
    std::vector<Blocker> found;
    if (!payload::InstallMachine())
        found.push_back({Tr("check.machine.title"),
                         Tr("check.machine.detail", {{L"machine", payload::MachineName(payload::NativeMachine())}})});
    if (!H264Available()) found.push_back({Tr("check.h264.title"), Tr("check.h264.detail")});
    if (!InstallDirAllowed(targetDir)) {
        found.push_back({Tr("check.folder.title"), Tr("check.folder.detail", {{L"root", ProgramFilesDir()}})});
    } else {
        // An update replaces files of about the same size; only a new install needs the room.
        const unsigned long long need = RequiredBytes(), free = FreeBytes(targetDir);
        const bool update = GetFileAttributesW((targetDir + L"\\animelogon.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
        auto mb = [](unsigned long long bytes) { return std::to_wstring((bytes + 1024 * 1024 - 1) / (1024 * 1024)) + L" MB"; };
        if (!update && free && free < need)
            found.push_back({Tr("check.space.title"), Tr("check.space.detail", {{L"need", mb(need)}, {L"free", mb(free)}})});
    }
    return found;
}

bool H264Available() {
    // Loaded by hand from System32: Windows N without the Media Feature Pack has no
    // mfplat.dll, and a static import would stop install.exe before it could say so.
    HMODULE mf = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mf) return false;
    const auto startup = reinterpret_cast<decltype(&MFStartup)>(GetProcAddress(mf, "MFStartup"));
    const auto shutdown = reinterpret_cast<decltype(&MFShutdown)>(GetProcAddress(mf, "MFShutdown"));
    const auto enumerate = reinterpret_cast<decltype(&MFTEnumEx)>(GetProcAddress(mf, "MFTEnumEx"));
    UINT32 count = 0;
    if (startup && shutdown && enumerate && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE))) {
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate **activates = nullptr;
        const HRESULT hr = enumerate(MFT_CATEGORY_VIDEO_DECODER,
                                     MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
                                         MFT_ENUM_FLAG_SORTANDFILTER,
                                     &input, nullptr, &activates, &count);
        if (FAILED(hr)) count = 0;
        if (activates) {
            for (UINT32 i = 0; i < count; ++i)
                if (activates[i]) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        shutdown();
    }
    FreeLibrary(mf);
    return count > 0;
}

std::wstring DefaultInstallDir() { return ProgramFilesDir() + L"\\AnimeLogon"; }

Report Install(const std::wstring &targetDir, const InstallOptions &options) {
    Report r;
    // Each step moves the bar as it starts, so that it moves before the slow ones.
    auto at = [&](double fraction, const char *key) {
        const std::wstring text = Tr(key);
        r.steps.push_back(text);
        ALOG(L"install: %s", text.c_str());
        if (options.progress) options.progress(fraction, text);
    };
    // An update that fails before its own service is installed starts the previous one again, if
    // it was running: until then the previous install is registered as it was, and it is better
    // that its logon screen runs than that Windows' lock screen stays off with nothing drawing it.
    bool restartOnFailure = false;
    auto fail = [&](const std::wstring &message) {
        r.message = message;
        ALOG(L"install: %s", message.c_str());
        if (restartOnFailure) {
            const DWORD started = machine::StartLauncherService();
            ALOG(L"install: the previous service %s (%lu)", started == ERROR_SUCCESS ? L"started again" : L"did not start again", started);
        }
        return r;
    };

    // The programs for the processor Windows runs on; a setup program without them installs nothing.
    const USHORT arch = payload::InstallMachine();
    if (!arch) {
        r.message = Tr("install.error.machine", {{L"machine", payload::MachineName(payload::NativeMachine())}});
        return r;
    }
    if (!H264Available()) {
        r.message = Tr("install.error.h264");
        return r;
    }
    if (!InstallDirAllowed(targetDir)) {
        r.message = Tr("install.error.dir", {{L"root", ProgramFilesDir()}, {L"dir", targetDir}});
        return r;
    }

    // What there was before, for an install that leaves switching on to the person.
    const bool fresh = machine::InstallDir().empty();
    const bool wasOn = !fresh && machine::IsOn();

    // Stop any previous install before touching its files.
    if (options.progress) options.progress(0.02, Tr("install.step.stop"));
    const DWORD before = machine::ServiceState();
    const DWORD stopped = machine::StopLauncherService();
    restartOnFailure = before == SERVICE_RUNNING || before == SERVICE_START_PENDING;

    // The data directory before anything is written into it, the log included.
    DWORD e = PrepareDataDir();
    if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.data"), e));
    log::Open(paths::LogPath(L"install.log"));
    ALOG(L"install: version %S for %s into %s (Windows runs on %s; this setup program is %s and carries %s)", ANIMELOGON_VERSION,
         payload::MachineName(arch).c_str(), targetDir.c_str(), payload::MachineName(payload::NativeMachine()).c_str(),
         payload::MachineName(payload::OwnMachine()).c_str(), [] {
             std::wstring list;
             for (USHORT m : payload::Carried()) list += (list.empty() ? L"" : L", ") + payload::MachineName(m);
             return list.empty() ? std::wstring(L"nothing, installing the files beside it") : list;
         }().c_str());
    if (stopped != ERROR_SUCCESS) ALOG(L"install: stopping the service failed (%lu)", stopped);
    if (restartOnFailure) ALOG(L"install: the previous service was running");
    at(0.10, "install.step.data");
    RemoveOldStores();

    at(0.15, "install.step.close_app");
    CloseRunningApp();

    at(0.20, "install.step.copy");
    if (!paths::CreateDirectories(targetDir)) return fail(Tr("install.error.mkdir", {{L"dir", targetDir}}));
    // An uninstall with no restart since left what it could not delete, uninstall.exe and the
    // directory at least, to be deleted at the next restart: that would now take the new copy.
    restart::KeepInstalled(targetDir);
    const std::vector<const wchar_t *> programs = payload::Names(payload::Kind::Program);
    size_t placed = 0;
    for (const wchar_t *program : programs) {
        e = Place(program, arch, targetDir + L"\\" + program, true);
        if (e == ERROR_FILE_NOT_FOUND) return fail(Tr("install.error.missing", {{L"name", program}}));
        if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.copy", {{L"name", program}}), e));
        if (options.progress) options.progress(0.20 + 0.40 * ++placed / programs.size(), r.steps.back());
    }
    paths::CreateDirectories(targetDir + L"\\licenses");
    for (const wchar_t *document : payload::Names(payload::Kind::Document)) {
        e = Place(document, arch, targetDir + L"\\" + document, false);
        if (e != ERROR_SUCCESS) ALOG(L"install: %s not written (%lu)", document, e);
    }
    RemoveDirectoryW((targetDir + L"\\licenses").c_str());  // only if nothing went into it
    for (const wchar_t *retired : kRetired) RemoveFileOrQueue(targetDir + L"\\" + retired);
    e = SceneReadAccess(targetDir,true);
    if (e == ERROR_SUCCESS) e = SceneReadAccess(targetDir+L"\\scene.exe",false);
    if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.scene_acl"),e));

    // The still the logon screen covers itself with until the overlay has baked the theme's. One
    // already in place is kept: the overlay may have baked it from the wallpaper.
    at(0.62, "install.step.background");
    const std::vector<uint8_t> png = background::Render();
    std::wstring why;
    if (!secure::IsTrusted(paths::BackgroundPath(), &why)) {
        e = secure::WriteBytes(paths::BackgroundPath(), png.data(), png.size());
        if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.background"), e));
    }

    // The default theme's wallpaper. Without it the default theme shows the built-in gradient,
    // so a failure is logged and the install goes on.
    at(0.66, "install.step.wallpaper");
    e = PlaceBundledWallpaper();
    if (e != ERROR_SUCCESS) ALOG(L"install: the bundled wallpaper not written (%lu)", e);

    // settings.ini holds only ids and enums, so Users may rewrite it. One an administrator did
    // not create is replaced with the defaults, and so is one from before themes: the default
    // theme, with nothing changed about it.
    at(0.70, "install.step.settings");
    bool defaults = !secure::IsAdminOwnedFile(paths::SettingsPath(), &why);
    if (defaults) ALOG(L"install: settings.ini %s -- writing defaults", why.c_str());
    else if ((defaults = !SettingsNameATheme())) ALOG(L"install: settings.ini names no theme -- writing defaults");
    if (defaults) {
        const std::string text = ToUtf8(SerializeSettings(Settings{}));
        e = secure::WriteBytes(paths::SettingsPath(), text.data(), text.size());
        if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.settings"), e));
    }
    e = secure::GrantUsersWrite(paths::SettingsPath());
    if (e != ERROR_SUCCESS) ALOG(L"install: settings.ini stays administrators-only (%lu)", e);

    // Recorded before Windows is touched, so that from here on a failure can be uninstalled.
    at(0.75, "install.step.register");
    e = machine::WriteInstallRecord(targetDir);
    if (e == ERROR_SUCCESS) e = WriteUninstallEntry(targetDir);
    if (e != ERROR_SUCCESS) return fail(WithError(Tr("install.error.register"), e));
    // The language the installer was shown in becomes the machine's: the settings app starts in it
    // until someone picks another, and uninstall.exe speaks it.
    if (!options.language.empty()) {
        e = i18n::SetMachineChoice(options.language);
        if (e != ERROR_SUCCESS) ALOG(L"install: language not recorded (%lu)", e);
    }
    // Theme packages open in the settings app, which imports them (assoc.h). Without it they are
    // still imported from the library, so a failure is logged and the install goes on.
    {
        const std::unique_ptr<assoc::Classes> classes = assoc::MachineClasses();
        e = assoc::Register(*classes, targetDir, Tr("install.file_type"));
        if (e != ERROR_SUCCESS) ALOG(L"install: .altheme files not registered (%lu)", e);
        else if (!assoc::IsDefault(*classes)) ALOG(L"install: another program opens .altheme files; AnimeLogon is offered under Open with");
        assoc::Notify();
    }

    // From here on the service is this install's: each failure below removes it and undoes what
    // was done to Windows, rather than starting it again.
    restartOnFailure = false;
    at(0.80, "install.step.service");
    e = machine::InstallService(LauncherPath(targetDir));
    if (e != ERROR_SUCCESS) {
        machine::RemoveService();
        return fail(WithError(Tr("install.error.service"), e));
    }

    std::wstring switchedOff;
    if (options.setupInApp) {
        // Windows and other programs stay as they are: the settings app switches AnimeLogon on
        // with the person, saying first what that changes. InstallService made the service start
        // with Windows, which only an AnimeLogon that is on does.
        if (wasOn) {
            e = machine::StartLauncherService();
            if (e != ERROR_SUCCESS) ALOG(L"install: the service did not start again (%lu)", e);
        } else {
            e = machine::SetServiceAutoStart(false);
            if (e != ERROR_SUCCESS) {
                machine::RemoveService();
                return fail(WithError(Tr("install.error.service"), e));
            }
            if (fresh) {
                e = machine::MarkSetupPending();
                if (e != ERROR_SUCCESS) ALOG(L"install: setup not marked pending (%lu)", e);
            }
        }
    } else {
        // Another program drawing the logon screen goes off before TurnOn records what Windows had.
        // One already off can still fail here, putting back Windows values it left (others.h).
        std::wstring failing;
        for (const others::Program &p : others::Installed()) {
            if (p.on) switchedOff = p.name;
            if (failing.empty() || p.on) failing = p.name;
        }
        if (!switchedOff.empty()) at(0.85, "install.step.others");
        e = others::TurnOff(paths::BackgroundPath());
        if (e != ERROR_SUCCESS) {
            machine::TurnOff();
            machine::RemoveService();
            return fail(WithError(Tr("install.error.others", {{L"name", failing}}), e));
        }
        r.switchedOff = switchedOff;

        at(0.88, "install.step.turn_on");
        e = machine::TurnOn(LauncherPath(targetDir));
        if (e != ERROR_SUCCESS) {
            // TurnOn has already put Windows' lock screen back; the service goes with it.
            machine::RemoveService();
            return fail(WithError(Tr("install.error.turn_on"), e));
        }
    }

    at(0.95, "install.step.shortcuts");
    const std::wstring old = Shortcut(FOLDERID_CommonPrograms, kOldShortcut);
    if (!old.empty()) DeleteFileW(old.c_str());
    const std::wstring link = Shortcut(FOLDERID_CommonPrograms, kShortcut);
    HRESULT hr = link.empty() ? E_FAIL : CreateShortcut(link, AppPath(targetDir), AppPath(targetDir));
    if (FAILED(hr)) ALOG(L"install: no Start menu shortcut (0x%08lx)", (unsigned long)hr);
    const std::wstring desktop = Shortcut(FOLDERID_PublicDesktop, kShortcut);
    if (options.desktopShortcut && !desktop.empty()) {
        hr = CreateShortcut(desktop, AppPath(targetDir), AppPath(targetDir));
        if (FAILED(hr)) ALOG(L"install: no desktop shortcut (0x%08lx)", (unsigned long)hr);
    }

    e = WriteRunEntry(targetDir);
    if (e != ERROR_SUCCESS) ALOG(L"install: no Run entry for the icon (%lu)", e);
    // Now too, for whoever is signed in here.
    const HRESULT tray = StartTrayOnDesktop(targetDir);
    if (tray == S_FALSE) ALOG(L"install: no shell on this desktop to start the icon in");
    else if (FAILED(tray)) ALOG(L"install: the icon was not started (0x%08lx)", (unsigned long)tray);

    r.ok = true;
    r.message = Tr(options.setupInApp && machine::SetupPending() ? "install.done_setup_in_app" : "install.done");
    if (!switchedOff.empty()) r.message += L"\n\n" + Tr("install.others_off", {{L"name", switchedOff}});
    if (options.progress) options.progress(1.0, Tr("install.step.done"));
    // The logon screen works without it; only the settings app needs it.
    if (!WebView2RuntimePresent(targetDir, arch)) {
        ALOG(L"install: no WebView2 runtime");
        r.webView2Missing = true;
        r.message += L"\n\n" + Tr("install.no_webview2");
    }
    return r;
}

Report Uninstall(bool keepData, const Progress &progress) {
    Report r;
    auto at = [&](double fraction, const char *key) {
        const std::wstring text = Tr(key);
        r.steps.push_back(text);
        ALOG(L"uninstall: %s", text.c_str());
        if (progress) progress(fraction, text);
    };
    std::vector<std::wstring> &problems = r.problems;
    auto problem = [&](const char *what, DWORD e) {
        problems.push_back(WithError(Tr(what), e));
        ALOG(L"uninstall: %S failed (%lu)", what, e);
    };

    const std::wstring dir = machine::InstallDir();

    at(0.05, "uninstall.step.close_app");
    CloseRunningApp();

    at(0.15, "uninstall.step.entry");
    const std::pair<REFKNOWNFOLDERID, const wchar_t *> links[] = {
        {FOLDERID_CommonPrograms, kShortcut}, {FOLDERID_CommonPrograms, kOldShortcut}, {FOLDERID_PublicDesktop, kShortcut}};
    for (const auto &[folder, name] : links) {
        const std::wstring link = Shortcut(folder, name);
        if (!link.empty() && !DeleteFileW(link.c_str())) {
            const DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) problem("uninstall.problem.shortcut", e);
        }
    }
    DWORD e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) problem("uninstall.problem.entry", e);
    e = RemoveRunEntry();
    if (e != ERROR_SUCCESS) problem("uninstall.problem.run", e);
    e = assoc::Unregister(*assoc::MachineClasses());
    if (e != ERROR_SUCCESS) problem("uninstall.problem.file_type", e);
    assoc::Notify();

    // Only the service can write these back, so before it goes.
    at(0.30, "uninstall.step.service");
    const DWORD signIn = machine::AskServiceToRestore();
    if (signIn != ERROR_SUCCESS) problem("uninstall.problem.sign_in", signIn);

    e = machine::RemoveService();
    if (e != ERROR_SUCCESS) problem("uninstall.problem.service", e);

    // Restore what Windows draws before removing files, so nothing points at a deleted file.
    at(0.55, "uninstall.step.lock_screen");
    const DWORD restored = machine::RestoreWindowsLockScreen();
    if (restored != ERROR_SUCCESS) problem("uninstall.problem.lock_screen", restored);

    if (!keepData) {
        at(0.70, "uninstall.step.data");
        e = secure::RemoveTree(paths::DataDir());
        if (e != ERROR_SUCCESS) problem("uninstall.problem.data", e);
    } else {
        // The themes, their wallpapers and components, and settings.ini stay for a later install.
        // What somebody's session last reported is not kept; the service deletes it when it
        // stops, and this covers a service that could not.
        secure::RemoveTree(paths::LogDir());
        RemoveOldStores();
        machine::ClearPausedMarker();
        DeleteFileW(live::Path().c_str());
    }
    // A failed restore keeps its recorded originals for the next uninstall.
    e = machine::RemoveInstallRecord(restored != ERROR_SUCCESS || signIn != ERROR_SUCCESS);
    if (e != ERROR_SUCCESS) problem("uninstall.problem.record", e);

    if (!dir.empty()) {
        at(0.85, "uninstall.step.files");
        CloseRunningApp();
        DWORD first = ERROR_SUCCESS;
        std::vector<const wchar_t *> files = payload::Names(payload::Kind::Program);
        files.insert(files.end(), std::begin(kRetired), std::end(kRetired));
        const std::vector<const wchar_t *> documents = payload::Names(payload::Kind::Document);
        files.insert(files.end(), documents.begin(), documents.end());
        for (const wchar_t *file : files) {
            const std::wstring path = dir + L"\\" + file;
            const bool there = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            e = RemoveFileOrQueue(path);
            if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
            // Still there after a successful delete: queued for the next restart.
            if (e == ERROR_SUCCESS && there && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) r.restartPending = true;
        }
        // Gone now if empty; otherwise after the restart that removes what is still in use
        // (uninstall.exe itself, usually). Queued after the files, so it is empty by then.
        for (const std::wstring &d : {dir + L"\\licenses", dir}) {
            if (RemoveDirectoryW(d.c_str())) continue;
            e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) continue;
            if (MoveFileExW(d.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) r.restartPending = true;
            else if (first == ERROR_SUCCESS) first = GetLastError();
        }
        if (first != ERROR_SUCCESS) problem("uninstall.problem.files", first);
    }
    if (progress) progress(1.0, Tr("uninstall.step.done"));

    // Another program drawing the logon screen stays off: say where it comes back on.
    std::wstring stillOff;
    for (const others::Program &p : others::Installed())
        if (!p.on) {
            r.othersOff.push_back(p.name);
            stillOff += L"\n\n" + Tr("uninstall.others_off", {{L"name", p.name}});
        }

    if (problems.empty()) {
        r.ok = true;
        r.message = Tr("uninstall.done") + stillOff;
        return r;
    }
    std::wstring list;
    for (size_t i = 0; i < problems.size(); ++i) list += (i ? Tr("list.separator") : L"") + problems[i];
    r.message = Tr("uninstall.partial", {{L"list", list}});
    if (restored != ERROR_SUCCESS) r.message += Tr("sentence.separator") + Tr("uninstall.partial.restore");
    r.message += stillOff;
    return r;
}

}  // namespace engine
