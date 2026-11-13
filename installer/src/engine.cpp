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

#include <iterator>
#include <vector>

#include "animelogon/background.h"
#include "animelogon/components.h"
#include "animelogon/live.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace engine {

// WebView2Loader.dll is the settings app's: it finds the Edge WebView2 runtime its page runs in.
const wchar_t *const kComponents[] = {L"overlay.exe", L"launcher.exe", L"animelogon.exe", L"scene.exe", L"WebView2Loader.dll",
                                      L"uninstall.exe"};
const size_t kComponentCount = ARRAYSIZE(kComponents);

namespace {

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
constexpr const wchar_t *kShortcut = L"AnimeLogon 设置.lnk";
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

std::wstring ProgramFilesDir() {
    const std::wstring dir = KnownFolder(FOLDERID_ProgramFiles);
    return dir.empty() ? std::wstring(L"C:\\Program Files") : dir;
}

std::wstring SourceDir() { return paths::ModuleDir(); }
std::wstring LauncherPath(const std::wstring &dir) { return dir + L"\\launcher.exe"; }
std::wstring AppPath(const std::wstring &dir) { return dir + L"\\animelogon.exe"; }

std::wstring StartMenuShortcut() {
    const std::wstring dir = KnownFolder(FOLDERID_CommonPrograms);
    return dir.empty() ? std::wstring() : dir + L"\\" + kShortcut;
}

std::wstring WithError(const std::wstring &text, DWORD e) { return text + L"（错误 " + std::to_wstring(e) + L"）"; }

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
// (CopyComponent moves their file aside). Returns whether the icon was among them.
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
        WriteRegString(key, L"DisplayVersion", FromUtf8("0.1.0")),
        WriteRegString(key, L"Publisher", L"Nomen (helloyork)"),
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

// The SYSTEM service runs from here, so every directory on the way must already be closed
// to everyone but administrators. Program Files is; an arbitrary --dir may not be.
bool InstallDirAllowed(const std::wstring &dir) {
    const std::wstring root = ProgramFilesDir();
    return paths::IsWithin(dir, root) && dir.size() > root.size();
}

constexpr size_t kMaxSettingsBytes = 256 * 1024;

// %ProgramData%\AnimeLogon and the directories in it, taken back and emptied if anyone but
// an administrator could have prepared them: the logs, and the stores themes are made of.
DWORD PrepareDataDir() {
    for (const std::wstring &dir : {paths::DataDir(), paths::LogDir(), WallpapersDir(), ComponentsDir(), ThemesDir()}) {
        const DWORD e = secure::SecureDirectory(dir);
        if (e != ERROR_SUCCESS) return e;
    }
    return ERROR_SUCCESS;
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

// Copies a component over the installed one. One still running -- the settings app or the tray
// icon in another account's session, which CloseRunningApp cannot reach -- is moved aside first
// (a running program's file can be renamed, not replaced) and deleted at the next restart; it
// goes on running until it ends, and the next start is the new one.
DWORD CopyComponent(const std::wstring &from, const std::wstring &to) {
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return ERROR_SUCCESS;
    const DWORD e = GetLastError();
    if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) return e;
    const std::wstring aside = to + L"." + std::to_wstring(GetTickCount64()) + L".old";
    if (!MoveFileExW(to.c_str(), aside.c_str(), 0)) return e;
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        const DWORD again = GetLastError();
        MoveFileExW(aside.c_str(), to.c_str(), 0);
        return again;
    }
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    ALOG(L"%s was in use: the old copy goes at the next restart", to.c_str());
    return ERROR_SUCCESS;
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
bool WebView2RuntimePresent(const std::wstring &targetDir) {
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

}  // namespace

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

Report Install(const std::wstring &targetDir) {
    Report r;
    auto step = [&](const wchar_t *s) { r.steps.push_back(s); ALOG(L"install: %s", s); };
    auto fail = [&](const std::wstring &message) {
        r.message = message;
        ALOG(L"install: %s", message.c_str());
        return r;
    };

    if (!H264Available()) {
        r.message = L"未找到 H.264 解码器。AnimeLogon 不支持 Windows N/KN 版本，或需要安装媒体功能包。";
        return r;
    }
    if (!InstallDirAllowed(targetDir)) {
        r.message = L"安装目录必须位于 " + ProgramFilesDir() + L" 之内：" + targetDir;
        return r;
    }

    // Stop any previous install before touching its files.
    const DWORD stopped = machine::StopLauncherService();

    // The data directory before anything is written into it, the log included.
    DWORD e = PrepareDataDir();
    if (e != ERROR_SUCCESS) {
        r.message = WithError(L"无法保护数据目录", e);
        return r;
    }
    log::Open(paths::LogPath(L"install.log"));
    ALOG(L"install: into %s", targetDir.c_str());
    step(L"检查 H.264 解码器");
    if (stopped != ERROR_SUCCESS) ALOG(L"install: stopping the service failed (%lu)", stopped);
    step(L"停止正在运行的服务");
    RemoveOldStores();
    step(L"准备数据目录");

    if (!paths::CreateDirectories(targetDir)) return fail(L"无法创建安装目录：" + targetDir);
    if (CloseRunningApp()) step(L"关闭通知区域图标");
    for (const wchar_t *component : kComponents) {
        const std::wstring from = SourceDir() + L"\\" + component;
        const std::wstring to = targetDir + L"\\" + component;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
            return fail(std::wstring(L"缺少组件：") + component);
        e = CopyComponent(from, to);
        if (e != ERROR_SUCCESS) return fail(WithError(std::wstring(L"无法复制 ") + component, e));
    }
    for (const wchar_t *retired : kRetired) RemoveFileOrQueue(targetDir + L"\\" + retired);
    e = SceneReadAccess(targetDir,true);
    if (e == ERROR_SUCCESS) e = SceneReadAccess(targetDir+L"\\scene.exe",false);
    if (e != ERROR_SUCCESS) return fail(WithError(L"无法为场景渲染器设置读取权限",e));
    step(L"复制程序文件");

    // The sign-in background. One already in place is kept -- the overlay may have baked it
    // from the video -- since every change costs one black credential screen.
    const std::vector<uint8_t> png = background::Render();
    std::wstring why;
    if (!secure::IsTrusted(paths::BackgroundPath(), &why)) {
        e = secure::WriteBytes(paths::BackgroundPath(), png.data(), png.size());
        if (e != ERROR_SUCCESS) return fail(WithError(L"无法写入背景图", e));
    }
    step(L"生成登录背景");

    // settings.ini holds only ids and enums, so Users may rewrite it. One an administrator did
    // not create is replaced with the defaults, and so is one from before themes: the default
    // theme, with nothing changed about it.
    bool defaults = !secure::IsAdminOwnedFile(paths::SettingsPath(), &why);
    if (defaults) ALOG(L"install: settings.ini %s -- writing defaults", why.c_str());
    else if ((defaults = !SettingsNameATheme())) ALOG(L"install: settings.ini names no theme -- writing defaults");
    if (defaults) {
        const std::string text = ToUtf8(SerializeSettings(Settings{}));
        e = secure::WriteBytes(paths::SettingsPath(), text.data(), text.size());
        if (e != ERROR_SUCCESS) return fail(WithError(L"无法写入默认设置", e));
    }
    e = secure::GrantUsersWrite(paths::SettingsPath());
    if (e != ERROR_SUCCESS) ALOG(L"install: settings.ini stays administrators-only (%lu)", e);
    step(L"写入默认设置");

    // Recorded before Windows is touched, so that from here on a failure can be uninstalled.
    e = machine::WriteInstallRecord(targetDir);
    if (e == ERROR_SUCCESS) e = WriteUninstallEntry(targetDir);
    if (e != ERROR_SUCCESS) return fail(WithError(L"无法注册安装信息", e));
    step(L"注册卸载项");

    e = machine::InstallService(LauncherPath(targetDir));
    if (e != ERROR_SUCCESS) {
        machine::RemoveService();
        return fail(WithError(L"无法安装服务", e));
    }
    step(L"安装服务");

    e = machine::TurnOn(LauncherPath(targetDir), paths::BackgroundPath());
    if (e != ERROR_SUCCESS) {
        // TurnOn has already put Windows' lock screen back; the service goes with it.
        machine::RemoveService();
        return fail(WithError(L"无法启用登录界面视频，已撤销对系统的更改", e));
    }
    step(L"关闭 Windows 锁屏并启动服务");

    const std::wstring link = StartMenuShortcut();
    const HRESULT hr = link.empty() ? E_FAIL : CreateShortcut(link, AppPath(targetDir), AppPath(targetDir));
    if (SUCCEEDED(hr)) step(L"创建开始菜单快捷方式");
    else ALOG(L"install: no Start menu shortcut (0x%08lx)", (unsigned long)hr);

    e = WriteRunEntry(targetDir);
    if (e == ERROR_SUCCESS) step(L"登录时显示通知区域图标");
    else ALOG(L"install: no Run entry for the icon (%lu)", e);
    // Now too, for whoever is signed in here.
    const HRESULT tray = StartTrayOnDesktop(targetDir);
    if (tray == S_FALSE) ALOG(L"install: no shell on this desktop to start the icon in");
    else if (FAILED(tray)) ALOG(L"install: the icon was not started (0x%08lx)", (unsigned long)tray);

    r.ok = true;
    r.message = L"AnimeLogon 已安装。按 Win + L 锁定屏幕即可查看，或打开 AnimeLogon 设置导入视频或图片。";
    // The logon screen works without it; only the settings app needs it.
    if (!WebView2RuntimePresent(targetDir)) {
        ALOG(L"install: no WebView2 runtime");
        r.message += L"\n\nAnimeLogon 设置需要 Microsoft Edge WebView2 运行时，这台电脑上没有找到。"
                     L"请从 Microsoft 网站安装「WebView2 运行时」后再打开设置。";
    }
    return r;
}

Report Uninstall(bool keepData) {
    Report r;
    auto step = [&](const wchar_t *s) { r.steps.push_back(s); ALOG(L"uninstall: %s", s); };
    std::vector<std::wstring> problems;
    auto problem = [&](const wchar_t *what, DWORD e) {
        problems.push_back(WithError(what, e));
        ALOG(L"uninstall: %s failed (%lu)", what, e);
    };

    const std::wstring dir = machine::InstallDir();

    const std::wstring link = StartMenuShortcut();
    if (!link.empty() && !DeleteFileW(link.c_str())) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) problem(L"删除开始菜单快捷方式", e);
    }
    DWORD e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) problem(L"删除卸载项", e);
    e = RemoveRunEntry();
    if (e != ERROR_SUCCESS) problem(L"删除登录时启动项", e);
    step(L"移除卸载项、启动项与快捷方式");

    // Only the service can write these back, so before it goes.
    const DWORD signIn = machine::AskServiceToRestore();
    if (signIn != ERROR_SUCCESS) problem(L"恢复登录屏幕背景设置", signIn);

    e = machine::RemoveService();
    if (e != ERROR_SUCCESS) problem(L"删除服务", e);
    step(L"停止并删除服务");

    // Restore what Windows draws before removing files, so nothing points at a deleted file.
    const DWORD restored = machine::RestoreWindowsLockScreen();
    if (restored != ERROR_SUCCESS) problem(L"恢复 Windows 锁屏设置", restored);
    else step(L"恢复 Windows 锁屏");

    if (!keepData) {
        e = secure::RemoveTree(paths::DataDir());
        if (e != ERROR_SUCCESS) problem(L"删除数据目录", e);
        else step(L"删除数据目录");
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
    if (e != ERROR_SUCCESS) problem(L"删除安装记录", e);

    if (!dir.empty()) {
        CloseRunningApp();
        DWORD first = ERROR_SUCCESS;
        std::vector<const wchar_t *> files(kComponents, kComponents + kComponentCount);
        files.insert(files.end(), std::begin(kRetired), std::end(kRetired));
        for (const wchar_t *file : files) {
            e = RemoveFileOrQueue(dir + L"\\" + file);
            if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
        }
        // Gone now if empty; otherwise after the restart that removes what is still in use
        // (uninstall.exe itself, usually). Queued after the files, so it is empty by then.
        if (!RemoveDirectoryW(dir.c_str())) {
            e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && !MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) &&
                first == ERROR_SUCCESS)
                first = GetLastError();
        }
        if (first != ERROR_SUCCESS) problem(L"删除程序文件", first);
        else step(L"删除程序文件");
    }

    if (problems.empty()) {
        r.ok = true;
        r.message = L"AnimeLogon 已卸载。";
        return r;
    }
    r.message = L"AnimeLogon 已卸载，但以下步骤未完成：";
    for (size_t i = 0; i < problems.size(); ++i) r.message += (i ? L"；" : L"") + problems[i];
    r.message += L"。";
    if (restored != ERROR_SUCCESS) r.message += L"原来的锁屏设置仍记录在注册表中，重新安装后再卸载即可恢复。";
    return r;
}

}  // namespace engine
