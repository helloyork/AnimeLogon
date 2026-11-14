#include "host.h"
#include "sceneimport.h"

#include <commctrl.h>
#include <commdlg.h>
#include <sddl.h>
#define SECURITY_WIN32
#include <security.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <cwchar>
#include <set>
#include <cmath>

#include "animelogon/bitmap.h"
#include "animelogon/blur.h"
#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/i18n.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/others.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/version.h"

#include "commit.h"
#include "devices.h"
#include "exporter.h"
#include "fonts.h"
#include "resources.h"
#include "tray.h"

// TaskDialogIndirect, for the question asked when the window is closed during an import. The
// program's manifest asks for version 6 of the common controls, which has it.
#pragma comment(lib, "comctl32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace app {

namespace {

constexpr wchar_t kClass[] = L"AnimeLogonApp";
constexpr wchar_t kOrigin[] = L"https://animelogon.invalid/";
// The wallpaper store, read-only, so the page can play a video wallpaper itself.
constexpr wchar_t kWallpaperHost[] = L"wallpapers.animelogon.invalid";
constexpr UINT kImportProgress = WM_APP + 10;
constexpr UINT kImportDone = WM_APP + 11;
constexpr UINT kPageMessage = WM_APP + 12;
constexpr UINT kWorkshopFound = WM_APP + 13;
constexpr UINT kReveal = WM_APP + 14;  // the icon asked: the window comes forward
constexpr UINT kReread = WM_APP + 15;  // the icon changed settings.ini

// A folder of Wallpaper Engine's wallpapers, read-only, so the page can show their previews and
// play their videos before they are imported: r0.workshop.animelogon.invalid for the first.
std::wstring WorkshopHost(size_t folder) { return L"r" + std::to_wstring(folder) + L".workshop.animelogon.invalid"; }

// The elevated helper's command line is kept short enough for any way Windows passes it on;
// installing more than fit takes another prompt.
constexpr size_t kMaxElevatedArgs = 2000;

// A file dialog's list of filters, as OPENFILENAME takes it: each name, then its patterns.
std::wstring Filters(std::initializer_list<std::pair<std::wstring, const wchar_t *>> entries) {
    std::wstring out;
    for (const auto &[name, patterns] : entries) {
        out += name;
        out += L'\0';
        out += patterns;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

// The same, for the types of what is imported (importable.h), named in the page's language.
std::wstring FiltersOf(const std::vector<importable::Filter> &entries) {
    std::wstring out;
    for (const importable::Filter &f : entries) {
        out += i18n::Tr(f.name);
        out += L'\0';
        out += f.patterns;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

// What each kind takes, as the page lists it: extensions without the dot.
template <size_t N>
void Extensions(json::Writer &w, const wchar_t *key, const wchar_t *const (&extensions)[N]) {
    w.Key(key).BeginArray();
    for (const wchar_t *e : extensions) w.String(e);
    w.EndArray();
}

std::wstring PickFile(HWND owner, const wchar_t *filter, const wchar_t *extension, bool save,
                      const std::wstring &suggested) {
    wchar_t buf[MAX_PATH * 4] = L"";
    wcsncpy_s(buf, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) ? std::wstring(buf) : std::wstring();
}

// A name as a file name: the characters Windows does not allow become underscores.
std::wstring FileNameFor(const std::wstring &name) {
    std::wstring out;
    for (wchar_t c : name) out += (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    return out.empty() ? i18n::Tr("theme.file_name") : out;
}

std::wstring FileStem(const std::wstring &path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

bool EndsWith(const std::wstring &text, const wchar_t *suffix) {
    const size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

// The entries of themes\ by id, loadable or not.
std::set<std::wstring> InstalledThemeIds() {
    std::set<std::wstring> ids;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ThemesDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return ids;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsWallpaperId(fd.cFileName)) ids.insert(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ids;
}

bool WriteBytes(const std::wstring &path, const std::string &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    return CloseHandle(h) && ok;
}

// Whether apps are set to dark in Settings > Personalisation > Colours.
bool AppsDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// One query parameter of a request's URI, or empty.
std::wstring Param(const std::wstring &query, const wchar_t *name) {
    const std::wstring key = std::wstring(name) + L"=";
    size_t at = 0;
    while (at < query.size()) {
        size_t end = query.find(L'&', at);
        if (end == std::wstring::npos) end = query.size();
        if (query.compare(at, key.size(), key) == 0) return query.substr(at + key.size(), end - at - key.size());
        at = end + 1;
    }
    return L"";
}

UINT ParamSize(const std::wstring &query, const wchar_t *name, UINT fallback, UINT most) {
    const UINT v = (UINT)wcstoul(Param(query, name).c_str(), nullptr, 10);
    return v ? std::min(v, most) : fallback;
}

// Whether `ref` names an installed video wallpaper.
bool IsVideo(const Library &library, const std::wstring &ref) {
    for (const WallpaperInfo &w : library.wallpapers())
        if (w.id == ref && !w.builtIn) return w.kind == WallpaperKind::Video;
    return false;
}

// What the clock says now, in the words the logon screen will use.
std::wstring ClockNow(const ClockStyle &style) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const RegionalFormat format = FormatFor(UserFormat(), style.locale);
    std::wstring text = FillText(L"{time}", format, style, now);
    const std::wstring ampm = FillText(L"{ampm}", format, style, now), date = FillText(L"{date}", format, style, now);
    if (!ampm.empty()) text += L" " + ampm;
    if (!date.empty()) text += L" · " + date;
    return text;
}

// The name Windows' sign-in screen shows for the person running this window: their full name
// when the account has one, else the account name.
std::wstring AccountName() {
    wchar_t name[256];
    ULONG size = ARRAYSIZE(name);
    if (GetUserNameExW(NameDisplay, name, &size) && name[0]) return name;
    DWORD plain = ARRAYSIZE(name);
    return GetUserNameW(name, &plain) ? name : L"";
}

// The file of their account picture as the sign-in screen draws it (192 px), or empty when they
// have none and Windows draws its default one.
std::wstring AccountPicture() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    BYTE buffer[256];
    DWORD size = 0;
    std::wstring sid;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        wchar_t *text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, &text)) {
            sid = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    if (sid.empty()) return L"";
    wchar_t file[MAX_PATH];
    DWORD bytes = sizeof(file);
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + sid;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Image192", RRF_RT_REG_SZ, nullptr, file, &bytes) != ERROR_SUCCESS)
        return L"";
    return GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES ? L"" : file;
}

// Where the window was when it was last closed, so that it opens there again: per person, and apart
// for a sandbox window. HKCU\Software\AnimeLogon, beside the person's language.
const wchar_t *PlacementValue() { return paths::Sandbox().empty() ? L"Window" : L"SandboxWindow"; }

bool LoadPlacement(WINDOWPLACEMENT *wp) {
    DWORD size = sizeof(*wp);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AnimeLogon", PlacementValue(), RRF_RT_REG_BINARY, nullptr, wp, &size) !=
            ERROR_SUCCESS ||
        size != sizeof(*wp) || wp->length != sizeof(*wp))
        return false;
    // Only onto a display that is still there, and at a size that is still a window.
    const RECT &r = wp->rcNormalPosition;
    return r.right - r.left >= 400 && r.bottom - r.top >= 300 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

void SavePlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnimeLogon", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, PlacementValue(), 0, REG_BINARY, reinterpret_cast<const BYTE *>(&wp), sizeof(wp));
    RegCloseKey(key);
}

// The program's icon at `px` pixels as a PNG, for the page's title bar: the icon file's own
// drawing for that size (res/animelogon.ico redraws the small ones), with its transparency.
std::vector<uint8_t> IconPng(int px) {
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(ANIMELOGON_ICON_ID), IMAGE_ICON, px, px, 0);
    if (!icon) return {};
    ICONINFO info{};
    std::vector<uint8_t> png;
    if (GetIconInfo(icon, &info) && info.hbmColor) {
        Picture p;
        p.width = p.height = px;
        p.bgra.resize((size_t)px * px * 4);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
        HDC dc = GetDC(nullptr);
        const int rows = GetDIBits(dc, info.hbmColor, 0, px, p.bgra.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows == px) {
            for (size_t i = 0; i < p.bgra.size(); i += 4)
                for (int c = 0; c < 3; ++c) p.bgra[i + c] = (uint8_t)(p.bgra[i + c] * p.bgra[i + 3] / 255);
            png = Png(p);
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    DestroyIcon(icon);
    return png;
}

}  // namespace

Host::Host() : library_(DiskStores()), pictures_(library_) { monitors_ = EnumerateMonitors(); }

Host::~Host() {
    if (worker_.joinable()) {
        // Closed during an import: the worker stops, and what it staged goes with it.
        cancel_ = true;
        worker_.join();
        for (const ImportJob &j : importJobs_) secure::RemoveTree(j.dir);
    }
    if (scanner_.joinable()) scanner_.join();
}

bool Host::Create(int show, const std::wstring &reveal) {
    show_ = show;
    reveal_ = reveal;
    dark_ = AppsDark();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(ANIMELOGON_ICON_ID));
    wc.hbrBackground = CreateSolidBrush(Frame::Background(dark_));
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return false;

    // Sized for the theme page: a preview beside a list. CW_USEDEFAULT keeps the position the
    // shell asks for. The window keeps the styles of one with a caption, so Windows still treats
    // it as one (snapping, the system menu, the animations); Frame takes the caption itself away.
    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(1240, dpi, 96), h = MulDiv(800, dpi, 96);
    hwnd_ = CreateWindowExW(0, kClass, WindowTitle().c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                            nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    // The icon asks by WM_COPYDATA, also when this window runs elevated and it does not.
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    SetIcons();
    frame_.Attach(hwnd_, dark_);
    // Where it was left, maximised or not; otherwise where the shell puts a new window. Whoever
    // started the program with a position of its own (STARTUPINFO) has the last word.
    STARTUPINFOW started{sizeof(started)};
    GetStartupInfoW(&started);
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!(started.dwFlags & STARTF_USEPOSITION) && LoadPlacement(&placement)) {
        const bool maximised = placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED);
        placement.showCmd = SW_HIDE;
        SetWindowPlacement(hwnd_, &placement);
        if (maximised && (show_ == SW_SHOWNORMAL || show_ == SW_SHOWDEFAULT || show_ == SW_SHOW)) show_ = SW_SHOWMAXIMIZED;
    }
    ShowWindow(hwnd_, show_);
    UpdateWindow(hwnd_);
    StartSteam();
    StartWebView();
    SetTimer(hwnd_,1,500,nullptr);
    // Opening the app brings the icon back, after "退出" in its menu too.
    if (library_.settings().tray) StartTray();
    return true;
}

int Host::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK Host::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto *self = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    auto *self = reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// Gives the window icons loaded at the sizes its monitor's DPI calls for, so the title bar and the
// taskbar use the drawing made for that size (the .ico has separate ones for 16 to 32 px) instead of
// scaling the class icon. Runs again when the window moves to a monitor with another DPI; the icons it
// replaces are its own, so it destroys them.
void Host::SetIcons() {
    const UINT dpi = GetDpiForWindow(hwnd_);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    auto set = [&](WPARAM which, int metric) {
        const int size = GetSystemMetricsForDpi(metric, dpi);
        auto *icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(ANIMELOGON_ICON_ID), IMAGE_ICON,
                                                   size, size, LR_DEFAULTCOLOR));
        if (!icon) return;
        auto *old = reinterpret_cast<HICON>(SendMessageW(hwnd_, WM_SETICON, which, reinterpret_cast<LPARAM>(icon)));
        if (old) DestroyIcon(old);
    };
    set(ICON_SMALL, SM_CXSMICON);
    set(ICON_BIG, SM_CXICON);
}

void Host::PageBackground() {
    ComPtr<ICoreWebView2Controller2> c2;
    if (!controller_ || FAILED(controller_.As(&c2))) return;
    const COLORREF c = Frame::Background(dark_);
    c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, GetRValue(c), GetGValue(c), GetBValue(c)});
}

LRESULT Host::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT answer = 0;
    if (frame_.Handle(msg, wp, lp, &answer)) return answer;
    switch (msg) {
    case WM_SETTINGCHANGE:
        // Light or dark switched in Windows' colour settings: the page follows by itself, the
        // frame and what shows before the page draws follow here.
        if (lp && CompareStringOrdinal(reinterpret_cast<LPCWSTR>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL &&
            AppsDark() != dark_) {
            dark_ = !dark_;
            frame_.Colours(dark_);
            PageBackground();
        }
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) scenePreviews_.clear();
        Fit();
        return 0;
    case WM_TIMER:
        spectra_.Idle();
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();) {
            if (GetTickCount64()-it->second.used > 1500) it=scenePreviews_.erase(it); else ++it;
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetIcons();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = reinterpret_cast<MINMAXINFO *>(lp);
        const UINT dpi = GetDpiForWindow(hwnd_);
        mm->ptMinTrackSize = {MulDiv(560, dpi, 96), MulDiv(480, dpi, 96)};
        return 0;
    }
    case kImportProgress:
        if (importing_) Busy(ImportLabel(), importPermille_.load() / 10);
        return 0;
    case kImportDone:
        FinishImport();
        return 0;
    case WM_COPYDATA:
        return OnAsked(*reinterpret_cast<const COPYDATASTRUCT *>(lp));
    case kReveal:
        Reveal();
        return 0;
    case kReread:
        Reread();
        return 0;
    case kWorkshopFound:
        FoundWorkshop();
        return 0;
    case kSteamLines:
        OnSteamLines();
        return 0;
    case kPageMessage:
        if (!inbox_.empty()) {
            const json::Value m = std::move(inbox_.front());
            inbox_.pop_front();
            if (!frame_.OnPage(m)) OnMessage(m);
        }
        return 0;
    case WM_CLOSE:
        // Closing cancels an import under way, which may have been converting a video for
        // minutes: that is asked first. The question is asked once, however often the window is
        // asked to close while it is up (the installer asks too).
        if (importing_) {
            if (askingToClose_ || !CloseCancelsImport()) return 0;
            if (importing_) cancel_ = true;  // it may have finished while the question was up
        }
        SavePlacement(hwnd_);
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        scenePreviews_.clear(); KillTimer(hwnd_,1);
        StopSteam();
        if (controller_) controller_->Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Host::Fit() {
    if (!controller_) return;
    RECT r{};
    GetClientRect(hwnd_, &r);
    controller_->put_Bounds(r);
}

void Host::StartWebView() {
    // Delay-loaded, so the elevated helper runs without it; it ships beside the program.
    if (!LoadLibraryExW(L"WebView2Loader.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR)) {
        MessageBoxW(hwnd_, i18n::Tr("app.error.loader").c_str(), L"AnimeLogon", MB_OK | MB_ICONWARNING);
        DestroyWindow(hwnd_);
        return;
    }
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version) {
        MessageBoxW(hwnd_, i18n::Tr("app.error.webview2").c_str(), L"AnimeLogon", MB_OK | MB_ICONWARNING);
        DestroyWindow(hwnd_);
        return;
    }
    CoTaskMemFree(version);
    // The browser's profile lives with the app's own files, not beside the program.
    paths::CreateDirectories(paths::UserDataDir());
    const std::wstring profile = paths::UserDataDir() + L"\\WebView2";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(result) || !env) {
                    ALOG(L"WebView2 environment failed: 0x%08X", (unsigned)result);
                    return S_OK;
                }
                env_ = env;
                return env_->CreateCoreWebView2Controller(
                    hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                               [this](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
                                   if (FAILED(result) || !controller) {
                                       ALOG(L"WebView2 controller failed: 0x%08X", (unsigned)result);
                                       return S_OK;
                                   }
                                   controller_ = controller;
                                   controller_->get_CoreWebView2(&view_);
                                   ComPtr<ICoreWebView2Settings> settings;
                                   if (SUCCEEDED(view_->get_Settings(&settings))) {
                                       settings->put_AreDefaultContextMenusEnabled(FALSE);
                                       settings->put_IsStatusBarEnabled(FALSE);
                                       settings->put_IsZoomControlEnabled(FALSE);
                                       ComPtr<ICoreWebView2Settings3> s3;
                                       if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                       // The page's bar is the window's title bar: where its CSS
                                       // says `app-region: drag`, WebView2 drags the window,
                                       // maximises it on a double click and opens the system menu
                                       // on a right click, as a caption does.
                                       ComPtr<ICoreWebView2Settings9> s9;
                                       if (SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);
                                       else ALOG(L"WebView2: no drag regions; the window moves only from the keyboard (Alt+Space, Win+arrows)");
                                   }
                                   PageBackground();
                                   // Media only: the page's own origin may play these files but
                                   // not read them.
                                   ComPtr<ICoreWebView2_3> view3;
                                   if (SUCCEEDED(view_.As(&view3)) &&
                                       FAILED(view3->SetVirtualHostNameToFolderMapping(
                                           kWallpaperHost, WallpapersDir().c_str(),
                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
                                       ALOG(L"WebView2: the wallpaper store cannot be mapped; videos show as stills");
                                   // Wallpaper Engine's folders likewise, for their previews and
                                   // videos before they are imported. Found once, here: a
                                   // mapping made after the page has loaded does not reach it.
                                   workshopFolders_ = workshop::Folders();
                                   for (size_t i = 0; view3 && i < workshopFolders_.size(); ++i)
                                       if (FAILED(view3->SetVirtualHostNameToFolderMapping(
                                               WorkshopHost(i).c_str(), workshopFolders_[i].path.c_str(),
                                               COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
                                           ALOG(L"WebView2: %s cannot be mapped; its wallpapers show without previews",
                                                workshopFolders_[i].path.c_str());
                                   MapSteamFixture(view_.Get());
                                   EventRegistrationToken token{};
                                   view_->AddWebResourceRequestedFilter((std::wstring(kOrigin) + L"*").c_str(),
                                                                        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                                   view_->add_WebResourceRequested(
                                       Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                           [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) {
                                               OnResource(args);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_WebMessageReceived(
                                       Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                           [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) {
                                               LPWSTR source = nullptr, text = nullptr;
                                               // Only the app's own page may ask for anything.
                                               if (SUCCEEDED(args->get_Source(&source)) && source &&
                                                   wcsncmp(source, kOrigin, wcslen(kOrigin)) == 0 &&
                                                   SUCCEEDED(args->get_WebMessageAsJson(&text)) && text) {
                                                   json::Value m;
                                                   // Answered from the window's message loop, not
                                                   // inside this event: while WebView2 is
                                                   // delivering it, ShellExecuteEx cannot start the
                                                   // elevated helper and shows an error box instead.
                                                   if (json::Parse(text, &m)) {
                                                       inbox_.push_back(std::move(m));
                                                       PostMessageW(hwnd_, kPageMessage, 0, 0);
                                                   }
                                               }
                                               CoTaskMemFree(source);
                                               CoTaskMemFree(text);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_NavigationStarting(
                                       Callback<ICoreWebView2NavigationStartingEventHandler>(
                                           [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
                                               LPWSTR uri = nullptr;
                                               if (SUCCEEDED(args->get_Uri(&uri)) &&
                                                   (!uri || wcsncmp(uri, kOrigin, wcslen(kOrigin)) != 0))
                                                   args->put_Cancel(TRUE);
                                               CoTaskMemFree(uri);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   view_->add_NewWindowRequested(
                                       Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                           [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
                                               args->put_Handled(TRUE);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
                                   Fit();
                                   frame_.Raise();
                                   view_->Navigate((std::wstring(kOrigin) + L"index.html").c_str());
                                   return S_OK;
                               })
                               .Get());
            })
            .Get());
    if (FAILED(hr)) ALOG(L"WebView2 could not start: 0x%08X", (unsigned)hr);
}

// --- talking to the page -------------------------------------------------------------------

void Host::Send(const json::Writer &message) {
    if (view_) view_->PostWebMessageAsJson(message.text().c_str());
}

void Host::SendState() {
    json::Writer w;
    w.BeginObject().Member(L"type", L"state").Member(L"clock", ClockNow(library_.settings().clock));
    w.Key(L"library").BeginObject();
    library_.WriteLibrary(w);
    w.EndObject();
    w.Key(L"open");
    library_.WriteOpen(w);
    w.EndObject();
    Send(w);
}

// Answered at once, since the icon waits for the answer; what it asks for is done from the
// message loop.
bool Host::OnAsked(const COPYDATASTRUCT &cd) {
    if (cd.dwData == kAskReread && cd.cbData == 0) {
        PostMessageW(hwnd_, kReread, 0, 0);
        return true;
    }
    if (cd.dwData != kAskShow || cd.cbData % sizeof(wchar_t) || cd.cbData > 32 * sizeof(wchar_t) || (cd.cbData && !cd.lpData))
        return false;
    const std::wstring what(static_cast<const wchar_t *>(cd.lpData), cd.cbData / sizeof(wchar_t));
    if (!IsShowTarget(what)) return false;
    reveal_ = what;
    PostMessageW(hwnd_, kReveal, 0, 0);
    return true;
}

// Comes forward, and has the page show what the icon asked for once it is there to.
void Host::Reveal() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    if (!ready_ || reveal_.empty()) return;
    json::Writer w;
    w.BeginObject().Member(L"type", L"show").Member(L"what", reveal_).EndObject();
    Send(w);
    reveal_.clear();
}

// settings.ini changed under the window (the icon put another theme on the logon screen).
void Host::Reread() {
    library_.Reload();
    if (!ready_) return;
    const bool open = !library_.open().empty();
    if (open) RenderPreview();
    SendState();
    if (open) SendPreview(0);
}

void Host::SendEnvironment() {
    json::Writer w;
    w.BeginObject().Member(L"type", L"environment");
    // A sandbox (paths.h) stands for an installed AnimeLogon that is on, whatever this machine has.
    const bool sandbox = !paths::Sandbox().empty();
    w.Member(L"on", sandbox || machine::IsOn()).Member(L"installed", sandbox || !machine::InstallDir().empty());
    // Installed without being switched on (Steam's install script): the page shows only its setup
    // wizard until the person has switched AnimeLogon on there.
    w.Member(L"setupPending", !sandbox && machine::SetupPending());
    // Other programs that draw the logon screen, which switching AnimeLogon on switches off.
    w.Key(L"others").BeginArray();
    if (!sandbox)
        for (const others::Program &p : others::Installed())
            w.BeginObject().Member(L"name", p.name).Member(L"on", p.on).EndObject();
    w.EndArray();
    w.Member(L"version", L"" ANIMELOGON_VERSION);
    // The language the page is shown in, and whether the person chose it or it follows Windows.
    w.Member(L"lang", i18n::Tag(i18n::Current())).Member(L"langChoice", i18n::UserChoice());
    // Who the sign-in screen would greet, for the lock-screen preview.
    w.Member(L"user", AccountName()).Member(L"accountPicture", !AccountPicture().empty());
    // Whether there are Wallpaper Engine wallpapers to offer; the page asks for them when it shows them.
    w.Member(L"wallpaperEngine", !workshopFolders_.empty());
    // The Steam edition, with the Steam Workshop: the discover page and the Workshop's themes.
    w.Member(L"steam", steam_ != nullptr);
    // The file types each kind of import takes, which the page lists where it asks for the kind.
    w.Key(L"importable").BeginObject();
    Extensions(w, L"video", importable::kVideos);
    Extensions(w, L"picture", importable::kPictures);
    Extensions(w, L"package", importable::kPackages);
    w.EndObject();
    w.Key(L"monitors").BeginArray();
    int n = 0;
    for (const MonitorInfo &m : monitors_) {
        w.BeginObject().Member(L"key", m.key).Member(L"name", i18n::Tr("monitor.name", {{L"n", std::to_wstring(++n)}})).Member(L"model", m.name);
        w.Member(L"left", (int)m.rect.left).Member(L"top", (int)m.rect.top);
        w.Member(L"width", (int)(m.rect.right - m.rect.left)).Member(L"height", (int)(m.rect.bottom - m.rect.top));
        w.Member(L"primary", m.primary).EndObject();
    }
    w.EndArray();
    w.Key(L"fonts").BeginArray();
    for (const fonts::Family &f : fonts::MachineFamilies())
        w.BeginObject().Member(L"name", f.name).Member(L"stored", f.stored).EndObject();
    w.EndArray();
    w.Key(L"devices").BeginArray();
    for (const devices::Output &d : devices::ListOutputs())
        w.BeginObject().Member(L"id", d.id).Member(L"name", d.name).EndObject();
    w.EndArray();
    w.EndObject();
    Send(w);
}

void Host::SendPreview(int seq) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"preview").Key(L"rev").Int(previewRev_).Key(L"seq").Int(seq);
    w.Member(L"width", (int)drawnW_).Member(L"height", (int)drawnH_).Member(L"layer", previewLayer_);
    // The panels left out of the picture, for the page to draw from frames that follow the sound.
    w.Key(L"spectra").BeginArray();
    for (const std::wstring &part : previewSpectra_) w.String(part);
    w.EndArray();
    w.Key(L"bounds").BeginArray();
    for (const Bound &b : bounds_) {
        w.BeginObject().Member(L"instance", b.instance).Member(L"key", b.key).Member(L"screen", (int)b.screen);
        w.Member(L"left", (double)b.rect.left).Member(L"top", (double)b.rect.top);
        w.Member(L"right", (double)b.rect.right).Member(L"bottom", (double)b.rect.bottom);
        if (b.holds) {
            w.Key(L"hold").BeginArray();
            for (float x : b.holdX) w.Number(x);
            for (float y : b.holdY) w.Number(y);
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray();
    // Several only when spanning: the displays, in the picture's pixels.
    w.Key(L"screens").BeginArray();
    for (const Screen &s : screens_) {
        w.BeginObject().Member(L"left", (int)s.rect.left).Member(L"top", (int)s.rect.top);
        w.Member(L"right", (int)s.rect.right).Member(L"bottom", (int)s.rect.bottom).Member(L"components", s.components);
        w.EndObject();
    }
    w.EndArray().EndObject();
    Send(w);
}

void Host::Toast(const std::wstring &text) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"toast").Member(L"text", text).EndObject();
    Send(w);
}

void Host::Problem(const std::wstring &text) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"problem").Member(L"text", text).EndObject();
    Send(w);
}

void Host::Busy(const std::wstring &label, int percent) {
    json::Writer w;
    w.BeginObject().Member(L"type", L"busy");
    if (label.empty()) w.Key(L"label").Null();
    else w.Member(L"label", label).Member(L"percent", percent);
    w.EndObject();
    Send(w);
}

bool Host::Save() {
    if (library_.Save()) {
        saveFailed_ = false;
        return true;
    }
    if (!saveFailed_) Problem(i18n::Tr("msg.save_failed"));
    saveFailed_ = true;
    return false;
}

// A live edit (a slider or a key still moving something) is drawn no wider than this: a large
// window's picture takes several times as long to draw and encode, and would trail the control.
// The page scales it to the stage; the full size is drawn once the gesture ends.
constexpr UINT kLiveWidth = 1280;

void Host::RenderPreview(bool live) {
    bounds_.clear();
    previewBytes_.clear();
    screens_.clear();
    previewLayer_ = false;
    drawnW_ = previewW_;
    drawnH_ = previewH_;
    if (live && drawnW_ > kLiveWidth) {
        drawnH_ = std::max(16u, (UINT)std::lround((double)previewH_ * kLiveWidth / previewW_));
        drawnW_ = kLiveWidth;
    }
    previewSpectra_.clear();
    if (library_.open().empty() || drawnW_ < 16 || drawnH_ < 16) return;
    theme::Theme t = library_.Edited(library_.open());
    // A video wallpaper plays in the page itself, under the components drawn here.
    scene::Document sceneDoc;
    previewLayer_ = library_.LoadScene(library_.open(),&sceneDoc) || (previewMotion_ && IsVideo(library_, t.wallpaper));
    screens_ = Wall(drawnW_, drawnH_);
    // While the page may move things, the panels that hold a spectrum are left out of the picture
    // and the page draws them from frames that follow the sound (livespectrum.h). Not on a wall of
    // displays, where one component can be drawn on several of them.
    if (previewMotion_ && screens_.empty() && library_.settings().components) {
        previewSpectra_ = spectra_.Panels(t);
        if (!previewSpectra_.empty()) t = spectra_.Without(t);
    }
    Picture p;
    if (pictures_.Theme(t, drawnW_, drawnH_, previewLayer_ ? Draw::ComponentsOnly : Draw::Everything, &p, &bounds_,
                        &screens_))
        previewBytes_ = previewLayer_ ? Png(p) : Bmp(p);
    ++previewRev_;
}

void Host::Edited(bool ok, bool live, int seq) {
    // The page's controls show the state, so it goes before the picture, which takes longer.
    if (!ok || !live) {
        if (ok) Save();
        SendState();
    }
    if (ok) RenderPreview(live);
    SendPreview(seq);
}

void Host::Shape(const std::wstring &themeId, UINT width, UINT *height) const {
    // The display the theme is on in per-monitor mode, otherwise the main one.
    const MonitorInfo *shown = nullptr;
    for (const MonitorInfo &m : monitors_) {
        if (!shown && m.primary) shown = &m;
        const auto it = library_.settings().screens.find(m.key);
        if (library_.settings().monitorMode == MonitorMode::PerMonitor && it != library_.settings().screens.end() &&
            it->second == themeId) {
            shown = &m;
            break;
        }
    }
    *height = PictureHeight(library_.settings(), monitors_, shown, width);
}

bool Host::SceneFrame(const std::wstring &id, UINT width, UINT height, double time, bool motion,
                     std::vector<uint8_t> *bytes, std::wstring *bounds) {
    if (width < 16 || height < 16 || height > 2400) return false;
    const auto key=id+L"/"+std::to_wstring(width)+L"x"+std::to_wstring(height);
    const auto signature=library_.Edited(id).wallpaper;
    if (!scenePreviews_.count(key) && scenePreviews_.size() >= 2) {
        auto oldest=scenePreviews_.begin();
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();++it) if (it->second.used < oldest->second.used) oldest=it;
        scenePreviews_.erase(oldest);
    }
    auto &entry=scenePreviews_[key]; entry.used=GetTickCount64();
    std::wstring why;
    if (entry.signature != signature) {
        if (!library_.stores().Store().LoadWallpaper(signature,&entry.wallpaper,nullptr) || entry.wallpaper.kind != WallpaperKind::Scene) return false;
        entry.preview=std::make_unique<scene::Preview>(); entry.signature=signature;
        entry.failed=!entry.preview->Open(entry.wallpaper,width,height,library_.settings().OverridesFor(id).wallpaperValues,&why);
        if (entry.failed) ALOG(L"preview: scene %s could not start (%s)",signature.c_str(),why.c_str());
    }
    const auto &wallpaper=entry.wallpaper;
    if (entry.failed || wallpaper.sceneFps == 0) {
        Picture still; if (pictures_.Wallpaper(wallpaper.id,library_.Edited(id).fit,width,height,&still)) *bytes=Bmp(still);
        return !bytes->empty();
    }
    scene::Inputs input; input.time=(float)time;
    entry.preview->SetValues(library_.settings().OverridesFor(id).wallpaperValues);
    SYSTEMTIME local{}; GetLocalTime(&local);
    input.day=(local.wHour*3600.f+local.wMinute*60.f+local.wSecond)/86400.f;
    const auto battery=data::ReadBattery(); input.battery=battery.percent < 0 ? -1.f : battery.percent/100.f;
    bool mediaReady=false; data::Snapshot snapshot; snapshot.media=previewMedia_.Get(library_.settings().media,&mediaReady);
    if (!mediaReady) return false;
    input.playing=(float)snapshot.media.state; data::Context context; context.snapshot=&snapshot;
    FILETIME utc{}; GetSystemTimeAsFileTime(&utc); context.now=((uint64_t)utc.dwHighDateTime<<32)|utc.dwLowDateTime;
    double progress=0; data::FractionOf(L"media.progress",context,&progress); input.progress=(float)progress;
    if (!entry.preview->Frame(input,motion,bytes,&why)) {
        if (entry.preview->Failed(&why)) { entry.failed=true; ALOG(L"preview: scene %s stopped (%s)",wallpaper.id.c_str(),why.c_str()); }
        return false;
    }
    json::Writer w; w.BeginArray(); size_t n=0;
    for (const auto &r : entry.preview->Bounds()) {
        ++n; if (r[2] <= 0 || r[3] <= 0) continue;
        w.BeginObject().Member(L"instance",L"wallpaper").Member(L"key",L"p"+std::to_wstring(n)).Member(L"screen",0);
        w.Member(L"left",(double)r[0]*width).Member(L"top",(double)r[1]*height);
        w.Member(L"right",(double)(r[0]+r[2])*width).Member(L"bottom",(double)(r[1]+r[3])*height).EndObject();
    }
    w.EndArray(); *bounds=w.text(); return true;
}

data::Snapshot Host::LockData(bool *ready) {
    data::Snapshot s;
    s.battery = data::ReadBattery();
    s.media = previewMedia_.Get(library_.settings().media, ready);
    return s;
}

void Host::OnResource(ICoreWebView2WebResourceRequestedEventArgs *args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    LPWSTR raw = nullptr;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&raw)) || !raw) return;
    std::wstring uri = raw;
    CoTaskMemFree(raw);
    if (uri.compare(0, wcslen(kOrigin), kOrigin) != 0) return;
    std::wstring path = uri.substr(wcslen(kOrigin)), query;
    const size_t q = path.find(L'?');
    if (q != std::wstring::npos) {
        query = path.substr(q + 1);
        path.resize(q);
    }

    std::vector<uint8_t> body;
    std::wstring type;
    std::wstring sceneBounds;
    std::wstring partBox;  // X-Part-Box: where a lone panel is, in fractions of the picture
    Picture p;
    if (WebFile(path, &body, &type)) {
        // One of the page's own files. The page itself goes out in the language chosen, so that
        // its first paint is in it (app/web/src/i18n.ts reads data-lang).
        if (type.rfind(L"text/html", 0) == 0) {
            std::string html(body.begin(), body.end());
            const std::string from = "<html lang=\"zh-CN\">";
            const size_t at = html.find(from);
            if (at != std::string::npos) {
                const i18n::Lang lang = i18n::Current();
                html.replace(at, from.size(), "<html lang=\"" + ToUtf8(i18n::Locale(lang)) + "\" data-lang=\"" + ToUtf8(i18n::Tag(lang)) + "\">");
                body.assign(html.begin(), html.end());
            }
        }
    } else if (path.rfind(L"scene/",0) == 0 && EndsWith(path,L".bmp") && !IsIconic(hwnd_)) {
        const auto id=path.substr(6,path.size()-10);
        if (library_.Entry(id)) {
            const UINT width=ParamSize(query,L"w",960,3840); UINT height=ParamSize(query,L"h",0,2400);
            if (!height) Shape(id,width,&height);
            const double time=wcstod(Param(query,L"t").c_str(),nullptr)/1000;
            int frameW = 0, frameH = 0;
            if (std::isfinite(time) && time >= 0 && time <= 86400 &&
                SceneFrame(id,width,height,time,Param(query,L"motion") == L"1",&body,&sceneBounds) &&
                bitmap::ReadSize(body.data(), body.size(), body.size(), &frameW, &frameH)) {
                // At the theme's brightness and blur, as the logon screen draws the scene; the frame's
                // own size is the one its header gives.
                const theme::Theme t = library_.Edited(id);
                uint8_t *pixels = body.data() + bitmap::kHeaderBytes;
                ApplyBrightness(pixels, (size_t)frameW * frameH, 4, t.brightness);
                blur::Apply(pixels, frameW, frameH, 4, blur::Sigma(t.blur, frameH));
            }
        }
        type=L"image/bmp";
    } else if (path == L"preview") {
        body = previewBytes_;
        type = previewLayer_ ? L"image/png" : L"image/bmp";
    } else if (path.rfind(L"lift/without/", 0) == 0 || path.rfind(L"lift/only/", 0) == 0) {
        // A component lifted out of the open theme's picture, so that a drag on the page can move
        // its pixels at once (app/web/src/preview.ts): the picture as the stage shows it but
        // without that component, or the component alone over nothing. The page lays them over
        // each other until the drag ends and the picture is drawn again.
        const bool alone = path.rfind(L"lift/only/", 0) == 0;
        const std::wstring id = path.substr(alone ? 10 : 13);
        const bool layer = alone || previewLayer_;
        if (!library_.open().empty() && previewW_ >= 16 && previewH_ >= 16) {
            theme::Theme t = library_.Edited(library_.open());
            // As the stage shows it: without the panels the page draws moving (RenderPreview).
            if (!alone && !previewSpectra_.empty()) t = spectra_.Without(t);
            bool found = false;
            for (theme::Instance &i : t.components) {
                if (i.id == id) found = i.visible;
                if (alone ? i.id != id : i.id == id) i.visible = false;
            }
            const std::vector<Screen> wall = Wall(previewW_, previewH_);
            if (found && pictures_.Theme(t, previewW_, previewH_, layer ? Draw::ComponentsOnly : Draw::Everything, &p, nullptr, &wall))
                body = layer ? Png(p) : Bmp(p);
        }
        type = layer ? L"image/png" : L"image/bmp";
    } else if (path.rfind(L"thumb/", 0) == 0 && EndsWith(path, L".bmp")) {
        const std::wstring id = path.substr(6, path.size() - 10);
        if (library_.Entry(id)) {
            const UINT w = ParamSize(query, L"w", 384, 1920);
            UINT h = 0;
            Shape(id, w, &h);
            const std::vector<Screen> wall = Wall(w, h);
            if (pictures_.Theme(library_.Edited(id), w, h,
                                library_.settings().components ? Draw::Everything : Draw::WallpaperOnly, &p, nullptr,
                                &wall))
                body = Bmp(p);
        }
        type = L"image/bmp";
    } else if (path.rfind(L"layer/", 0) == 0 && EndsWith(path, L".png")) {
        // A theme's components alone, to lay over its video wallpaper playing in the page.
        const std::wstring id = path.substr(6, path.size() - 10);
        if (library_.Entry(id) && library_.settings().components) {
            const UINT w = ParamSize(query, L"w", 960, 3840);
            UINT h = ParamSize(query, L"h", 0, 3840);
            const bool shaped = h == 0;
            if (shaped) Shape(id, w, &h);
            const std::vector<Screen> wall = shaped ? Wall(w, h) : std::vector<Screen>();
            if (pictures_.Theme(library_.Edited(id), w, h, Draw::ComponentsOnly, &p, nullptr, &wall)) body = Png(p);
        }
        type = L"image/png";
    } else if (path.rfind(L"wallpaper/", 0) == 0 && EndsWith(path, L".bmp")) {
        const std::wstring ref = path.substr(10, path.size() - 14);
        if (IsWallpaperRef(ref) && pictures_.Wallpaper(ref, Scaling::Fill, 256, 144, &p)) body = Bmp(p);
        type = L"image/bmp";
    } else if (path.rfind(L"lock/", 0) == 0 && EndsWith(path, L"/entrance.json")) {
        // How each panel of a theme's visible components comes in, for the lock-screen preview
        // to play with one picture per panel (below): the same values the logon screen reads.
        // `ready` is false until what is playing has been read, and the pictures would not yet
        // show it: the page asks again. A panel of a component that shows what is playing is
        // `live`: the page draws it again every second, as the logon screen moves its progress
        // bar and follows a song starting, changing or stopping.
        const std::wstring id = path.substr(5, path.size() - 5 - 14);
        bool ready = false;
        LockData(&ready);
        json::Writer w;
        w.BeginObject().Member(L"ready", ready).Key(L"parts").BeginArray();
        if (library_.Entry(id) && library_.settings().components) {
            const theme::Theme t = library_.Edited(id);
            for (const theme::Instance &i : t.components) {
                skin::Skin skin;
                if (!i.visible || !library_.LoadSkin(i.ref, &skin)) continue;
                skin::Values values;
                for (const auto &[key, value] : i.sets)
                    if (skin::Accepts(skin, key, value)) values[key] = value;
                const skin::Resolved r = skin::Resolve(skin, values);
                const std::vector<data::Source> sources = skin::SourcesOf(skin);
                const bool live = std::find(sources.begin(), sources.end(), data::Source::Media) != sources.end();
                for (size_t k = 0; k < r.panels.size(); ++k) {
                    const skin::Panel &panel = r.panels[k];
                    // A panel that holds a spectrum the page draws from frames that follow the sound
                    // (spectrum/..., below), rather than from one picture.
                    bool audio = false;
                    for (const skin::Line &l : panel.lines)
                        for (const skin::Text &x : l.texts) audio = audio || x.spectrum;
                    const wchar_t *how = panel.enter == skin::Enter::Rise   ? L"rise"
                                         : panel.enter == skin::Enter::Zoom ? L"zoom"
                                         : panel.enter == skin::Enter::None ? L"none"
                                                                        : L"fade";
                    w.BeginObject().Member(L"part", i.id + L".p" + std::to_wstring(k + 1)).Member(L"enter", how);
                    w.Member(L"delay", (double)panel.enterDelay).Member(L"duration", (double)panel.enterDuration);
                    w.Member(L"size", (double)panel.size).Member(L"live", live).Member(L"audio", audio).EndObject();
                }
            }
        }
        w.EndArray().EndObject();
        const std::string text = ToUtf8(w.text());
        body.assign(text.begin(), text.end());
        type = L"application/json";
    } else if (path.rfind(L"lock/", 0) == 0 && path.find(L"/part/") != std::wstring::npos && EndsWith(path, L".png")) {
        // One panel of one component, alone over nothing: lock/<theme>/part/<instance>.<panel>.png.
        const size_t at = path.find(L"/part/");
        const std::wstring id = path.substr(5, at - 5), part = path.substr(at + 6, path.size() - at - 6 - 4);
        if (library_.Entry(id) && library_.settings().components) {
            const UINT w = ParamSize(query, L"w", 960, 3840);
            UINT h = ParamSize(query, L"h", 0, 3840);
            const bool shaped = h == 0;
            if (shaped) Shape(id, w, &h);
            const std::vector<Screen> wall = shaped ? Wall(w, h) : std::vector<Screen>();
            std::vector<Bound> placed;
            const data::Snapshot now = LockData(nullptr);
            if (pictures_.Theme(library_.Edited(id), w, h, Draw::ComponentsOnly, &p, &placed, &wall, part, &now)) {
                body = Png(p);
                for (const Bound &b : placed)
                    if (b.holds) {
                        wchar_t box[96];
                        swprintf_s(box, L"%.5f,%.5f,%.5f,%.5f", b.rect.left / w, b.rect.top / h, b.rect.right / w,
                                   b.rect.bottom / h);
                        partBox = box;
                        break;
                    }
            }
        }
        type = L"image/png";
    } else if (path.rfind(L"spectrum/", 0) == 0 && EndsWith(path, L".png") && !IsIconic(hwnd_)) {
        // One panel that holds a spectrum, as it is this moment (livespectrum.h), cut to what it can
        // touch: spectrum/<theme>/<instance>.p<n>.png. The theme page and the lock-screen preview
        // (`lock=1`, with the battery and what is playing as they are) ask about thirty times a
        // second; X-Part-Box says where the picture goes.
        const size_t slash = path.find(L'/', 9);
        const std::wstring part = slash == std::wstring::npos ? L"" : path.substr(slash + 1, path.size() - slash - 1 - 4);
        const size_t dot = part.rfind(L".p");
        if (slash != std::wstring::npos && dot != std::wstring::npos) {
            const std::wstring id = path.substr(9, slash - 9), instance = part.substr(0, dot);
            const size_t panel = (size_t)_wtoi(part.c_str() + dot + 2);
            if (library_.Entry(id) && library_.settings().components) {
                const UINT w = ParamSize(query, L"w", 960, 3840);
                UINT h = ParamSize(query, L"h", 0, 3840);
                if (!h) Shape(id, w, &h);
                const bool lock = Param(query, L"lock") == L"1";
                FILETIME utc{};
                GetSystemTimeAsFileTime(&utc);
                const data::Snapshot data = lock ? LockData(nullptr) : data::Sample((uint64_t)utc.dwHighDateTime << 32 | utc.dwLowDateTime);
                RECT where{};
                if (h >= 16 && spectra_.Frame(lock ? L"lock" : L"stage", library_.Edited(id), instance, panel, w, h, data, &p, &where)) {
                    body = Png(p);
                    wchar_t box[96];
                    swprintf_s(box, L"%.5f,%.5f,%.5f,%.5f", (double)where.left / w, (double)where.top / h,
                               (double)where.right / w, (double)where.bottom / h);
                    partBox = box;
                }
            }
        }
        type = L"image/png";
    } else if (path == L"icon") {
        body = IconPng((int)ParamSize(query, L"px", 16, 256));
        type = L"image/png";
    } else if (path == L"account") {
        // The person's account picture, for the lock-screen preview's sketch of the sign-in screen.
        const std::wstring file = AccountPicture();
        if (!file.empty() && secure::ReadFileBytes(file, &body, 8 * 1024 * 1024))
            type = EndsWith(file, L".png") || EndsWith(file, L".PNG") ? L"image/png" : L"image/jpeg";
        else
            body.clear();
    } else if (path.rfind(L"lock/", 0) == 0 && (EndsWith(path, L"/still.bmp") || EndsWith(path, L"/full.bmp"))) {
        // The lock-screen preview: the wallpaper first, then the components over it.
        const bool full = EndsWith(path, L"/full.bmp");
        const std::wstring id = path.substr(5, path.size() - 5 - (full ? 9 : 10));
        if (library_.Entry(id)) {
            // As the theme's display (or wall of displays) shows it, or as one display of another
            // shape would.
            const UINT w = ParamSize(query, L"w", 1920, 3840);
            UINT h = ParamSize(query, L"h", 0, 3840);
            const bool shaped = h == 0;
            if (shaped) Shape(id, w, &h);
            const std::vector<Screen> wall = shaped ? Wall(w, h) : std::vector<Screen>();
            const theme::Theme t = library_.Edited(id);
            const data::Snapshot now = LockData(nullptr);
            if (pictures_.Theme(t, w, h, full && library_.settings().components ? Draw::Everything : Draw::WallpaperOnly, &p,
                                nullptr, &wall, L"", &now))
                body = Bmp(p);
        }
        type = L"image/bmp";
    }

    ComPtr<IStream> stream;
    ComPtr<ICoreWebView2WebResourceResponse> response;
    const std::wstring headers = L"Content-Type: " + type + L"\r\nCache-Control: no-store"+
        (sceneBounds.empty() ? L"" : L"\r\nX-Scene-Bounds: "+sceneBounds) +
        (partBox.empty() ? L"" : L"\r\nX-Part-Box: " + partBox);
    if (!body.empty()) stream.Attach(SHCreateMemStream(body.data(), (UINT)body.size()));
    if (stream) env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    else env_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    if (response) args->put_Response(response.Get());
}

void Host::OnMessage(const json::Value &m) {
    if (m[L"op"].Str() == L"sceneStop") {
        const auto id=m[L"theme"].Str(); const UINT width=(UINT)m[L"width"].Num(); UINT height=(UINT)m[L"height"].Num();
        if (!height && width && library_.Entry(id)) Shape(id,width,&height);
        const auto key=id+L"/"+std::to_wstring(width)+L"x"+std::to_wstring(height);
        for (auto it=scenePreviews_.begin();it!=scenePreviews_.end();) {
            if (it->first == key) it=scenePreviews_.erase(it); else ++it;
        }
        return;
    }
    const std::wstring op = m[L"op"].Str();
    if (op.rfind(L"steam", 0) == 0 && OnSteamMessage(op, m)) return;
    const int seq = (int)m[L"seq"].Num();
    const int gesture = (int)m[L"gesture"].Num();
    const bool live = m[L"live"].Bool();
    const std::wstring instance = m[L"instance"].Str();

    if (op == L"ready") {
        SendEnvironment();
        SendState();
        ready_ = true;
        if (!reveal_.empty()) Reveal();
    } else if (op == L"refresh") {
        SendState();
    } else if (op == L"open") {
        library_.Open(m[L"theme"].Str());
        RenderPreview();
        SendState();
        SendPreview(seq);
    } else if (op == L"close") {
        library_.Close();
        SendState();
    } else if (op == L"apply") {
        const std::wstring id = m[L"theme"].Str();
        if (library_.Apply(id, m[L"monitor"].Str()) && Save())
            if (const ThemeEntry *e = library_.Entry(id)) Toast(i18n::Tr("msg.applied", {{L"name", e->name}}));
        SendState();
    } else if (op == L"preview") {
        previewW_ = (UINT)std::clamp(m[L"width"].Num(), 0.0, 3840.0);
        previewH_ = (UINT)std::clamp(m[L"height"].Num(), 0.0, 2400.0);
        previewMotion_ = m[L"motion"].Bool();
        RenderPreview();
        SendPreview(seq);
    } else if (op == L"set") {
        Edited(library_.SetValue(instance, m[L"key"].Str(), m[L"value"].Str(), gesture), live, seq);
    } else if (op == L"setMany") {
        // Values set together are one step to undo, also outside a drag.
        static int together = 0;
        const int step = gesture ? gesture : --together;
        bool ok = true;
        for (const auto &[key, value] : m[L"values"].Members())
            ok = library_.SetValue(instance, key, value.Str(), step) && ok;
        Edited(ok, live, seq);
    } else if (op == L"clear") {
        std::vector<std::wstring> keys;
        for (const json::Value &k : m[L"keys"].Items()) keys.push_back(k.Str());
        Edited(library_.ClearValues(instance, keys), false, seq);
    } else if (op == L"visible") {
        Edited(library_.SetVisible(instance, m[L"on"].Bool()), false, seq);
    } else if (op == L"add") {
        const std::wstring id = library_.AddInstance(m[L"component"].Str());
        if (!id.empty()) {
            json::Writer w;
            w.BeginObject().Member(L"type", L"added").Member(L"instance", id).EndObject();
            Send(w);
        }
        Edited(!id.empty(), false, seq);
    } else if (op == L"removeInstance") {
        Edited(library_.RemoveInstance(instance), false, seq);
    } else if (op == L"copyComponents") {
        // The open theme is unchanged, so its picture stays; the targets' pictures follow their
        // revisions in the state.
        std::vector<std::wstring> targets;
        for (const json::Value &t : m[L"themes"].Items()) targets.push_back(t.Str());
        const int copied = library_.CopyComponents(targets);
        if (copied < 0) Problem(i18n::Tr("msg.components_too_large"));
        else if (copied > 0 && Save()) {
            const ThemeEntry *one = nullptr;
            for (const std::wstring &t : targets)
                if (copied == 1 && t != library_.open() && (one = library_.Entry(t))) break;
            Toast(one ? i18n::Tr("msg.components_copied_one", {{L"name", one->name}})
                      : i18n::Tr("msg.components_copied", {{L"n", std::to_wstring(copied)}}));
        }
        SendState();
    } else if (op == L"wallpaper") {
        Edited(library_.SetWallpaper(m[L"ref"].Str()), false, seq);
    } else if (op == L"wake") {
        WakeStyle style = WakeStyle::Fade;
        Edited(Parse(m[L"value"].Str(), &style) && library_.SetWake(style), false, seq);
    } else if (op == L"wakeSeconds") {
        Edited(library_.SetWakeSeconds((float)m[L"value"].Num(), gesture), false, seq);
    } else if (op == L"fit") {
        Scaling fit;
        Edited(Parse(m[L"value"].Str(), &fit) && library_.SetFit(fit), false, seq);
    } else if (op == L"brightness") {
        Edited(library_.SetBrightness((float)m[L"value"].Num(), gesture), false, seq);
    } else if (op == L"blur") {
        Edited(library_.SetBlur((float)m[L"value"].Num(), gesture), false, seq);
    } else if (op == L"reset") {
        Edited(library_.Reset(), false, seq);
    } else if (op == L"undo") {
        Edited(library_.Undo(), false, seq);
    } else if (op == L"redo") {
        Edited(library_.Redo(), false, seq);
    } else if (op == L"commit") {
        Edited(true, false, seq);
    } else if (op == L"global") {
        const std::wstring key = m[L"key"].Str();
        if (library_.SetGlobal(key, m[L"value"].Str()) && Save() && key == L"tray") {
            if (library_.settings().tray) StartTray();
            else CloseTray();
        }
        if (!library_.open().empty()) RenderPreview();
        SendState();
        if (!library_.open().empty()) SendPreview(seq);
    } else if (op == L"import") {
        // The kind chosen on the page; a message without one gets every kind, as before.
        Import(false, importable::KindNamed(m[L"kind"].Str()));
    } else if (op == L"importWallpaper") {
        Import(true);
    } else if (op == L"cancelImport") {
        cancel_ = true;
    } else if (op == L"workshop") {
        FindWorkshop();
    } else if (op == L"importWorkshop") {
        ImportWorkshop(m[L"items"], m[L"wallpaper"].Bool());
    } else if (op == L"importComponent") {
        ImportComponent(m[L"add"].Bool());
    } else if (op == L"newTheme") {
        NewTheme(m[L"name"].Str(), m[L"wallpaper"].Str());
    } else if (op == L"copy") {
        CopyTheme(m[L"theme"].Str(), m[L"name"].Str());
    } else if (op == L"rename") {
        RenameTheme(m[L"theme"].Str(), m[L"name"].Str());
    } else if (op == L"remove") {
        RemoveTheme(m[L"theme"].Str());
    } else if (op == L"export") {
        ExportTheme(m[L"theme"].Str());
    } else if (op == L"switch") {
        Switch(m[L"on"].Bool());
    } else if (op == L"logs") {
        ShellExecuteW(hwnd_, L"open", paths::LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (op == L"language") {
        // The person's own choice, or none to follow Windows again; everything is said again in it.
        const std::wstring tag = m[L"tag"].Str();
        i18n::Lang lang;
        if (!tag.empty() && !i18n::FromTag(tag, &lang)) return;
        const DWORD e = i18n::SetUserChoice(tag);
        if (e != ERROR_SUCCESS) ALOG(L"language: not stored (%lu)", e);
        i18n::Use(i18n::Preferred());
        if (!paths::Sandbox().empty()) {
            SetWindowTextW(hwnd_, paths::SandboxTitle().c_str());
            frame_.Tell();
        }
        SendEnvironment();
        SendState();
    } else if (op == L"link") {
        // The project's pages and the licences, by name: the page cannot open anything else.
        const std::wstring to = m[L"to"].Str(), dir = machine::InstallDir();
        const std::wstring project = L"https://github.com/helloyork/AnimeLogon";
        std::wstring target;
        if (to == L"home") target = project;
        else if (to == L"issues") target = project + L"/issues";
        else if (to == L"releases") target = project + L"/releases";
        else if (to == L"license")
            target = !dir.empty() && GetFileAttributesW((dir + L"\\LICENSE.txt").c_str()) != INVALID_FILE_ATTRIBUTES
                         ? dir + L"\\LICENSE.txt" : project + L"/blob/master/LICENSE";
        else if (to == L"notices")
            target = !dir.empty() && GetFileAttributesW((dir + L"\\licenses").c_str()) != INVALID_FILE_ATTRIBUTES
                         ? dir + L"\\licenses" : project + L"/tree/master/vendor";
        if (!target.empty()) ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (op == L"uninstall") {
        if (!paths::Sandbox().empty()) {
            Problem(i18n::Tr("msg.sandbox.uninstall"));
            return;
        }
        const std::wstring dir = machine::InstallDir();
        if (!dir.empty()) ShellExecuteW(hwnd_, L"runas", (dir + L"\\uninstall.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// --- what needs the elevated helper ----------------------------------------------------------

bool Host::RefuseWhileImporting() {
    if (!importing_) return false;
    Toast(i18n::Tr("msg.import_busy"));
    return true;
}

bool Host::CloseCancelsImport() {
    const std::wstring instruction = i18n::Tr("app.close_importing"), detail = i18n::Tr("app.close_importing.detail");
    const std::wstring close = i18n::Tr("app.close_importing.close"), keep = i18n::Tr("app.close_importing.keep");
    constexpr int kClose = 100, kKeep = 101;
    const TASKDIALOG_BUTTON buttons[] = {{kClose, close.c_str()}, {kKeep, keep.c_str()}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hwndParent = hwnd_;
    // Esc and the title bar's close button keep the window, as Keep importing does.
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"AnimeLogon";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = detail.c_str();
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = kKeep;
    int pressed = kKeep;
    askingToClose_ = true;
    const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    askingToClose_ = false;
    if (FAILED(hr)) ALOG(L"close: the question could not be asked (0x%08lX)", (unsigned long)hr);
    return SUCCEEDED(hr) && pressed == kClose;
}

void Host::Import(bool wallpaper, importable::Kind kind) {
    if (RefuseWhileImporting()) return;
    const std::vector<importable::Filter> filters = wallpaper ? importable::WallpaperFilters() : importable::ThemeFilters(kind);
    ALOG(L"import: %s, file dialog opens on %s", wallpaper ? L"wallpaper" : importable::NameOf(kind), filters[0].patterns.c_str());
    const std::wstring source = PickFile(hwnd_, FiltersOf(filters).c_str(), nullptr, false, L"");
    if (source.empty()) return;
    StartImport({{source, L"", L""}}, wallpaper);
}

void Host::StartImport(std::vector<ImportJob> jobs, bool wallpaper) {
    if (jobs.empty() || RefuseWhileImporting()) return;
    for (ImportJob &j : jobs) {
        j.dir = staging::NewImportDir();
        if (j.dir.empty()) {
            for (const ImportJob &made : jobs)
                if (!made.dir.empty()) secure::RemoveTree(made.dir);
            Problem(i18n::Tr("msg.temp_dir", {{L"path", paths::UserDataDir() + L"\\import"}}));
            return;
        }
    }
    // A scene brought in for a theme's wallpaper comes as a theme of its own.
    importWallpaper_ = wallpaper && jobs.size() == 1 && !EndsWith(jobs[0].source, L".alscene");
    importJobs_ = jobs;
    importResults_.clear();
    cancel_ = false;
    importing_ = true;
    importPermille_ = 0;
    importIndex_ = 0;
    Busy(ImportLabel(), 0);
    worker_ = std::thread([this, jobs = std::move(jobs), wallpaper] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<staging::Result> results(jobs.size());
        for (size_t i = 0; i < jobs.size() && !cancel_; ++i) {
            const ImportJob &j = jobs[i];
            importIndex_ = (int)i;
            // The bar goes once across for all of them.
            auto progressed = [this, i, n = jobs.size()](double f) {
                importPermille_ = (int)((i + std::clamp(f, 0.0, 1.0)) * 1000 / n);
                PostMessageW(hwnd_, kImportProgress, 0, 0);
            };
            progressed(0);
            if (FAILED(co)) {
                results[i].error = i18n::Tr("msg.com");
                continue;
            }
            const bool scene = EndsWith(j.source, L".alscene");
            const bool package = !wallpaper && EndsWith(j.source, L".altheme");
            results[i] = scene       ? staging::Scene(j.source, j.dir, cancel_, progressed)
                         : package   ? staging::Package(j.source, j.dir, cancel_, progressed, j.name)
                         : wallpaper ? staging::Wallpaper(j.source, j.dir, cancel_, progressed, j.name)
                                     : staging::Media(j.source, j.dir, cancel_, progressed, j.name);
        }
        if (SUCCEEDED(co)) CoUninitialize();
        importResults_ = std::move(results);
        PostMessageW(hwnd_, kImportDone, 0, 0);
    });
}

// What the busy bar says: the file being staged now, and which of how many when there are several.
std::wstring Host::ImportLabel() const {
    if (importJobs_.empty()) return L"";
    const size_t i = std::min<size_t>((size_t)importIndex_.load(), importJobs_.size() - 1);
    const ImportJob &j = importJobs_[i];
    const std::wstring name = j.name.empty() ? FileStem(j.source) : j.name;
    std::wstring label = i18n::Tr(!importWallpaper_ && EndsWith(j.source, L".altheme") ? "busy.reading"
                                  : staging::LooksLikePicture(j.source)                ? "busy.processing"
                                                                                       : "busy.transcoding",
                                  {{L"name", name}});
    if (importJobs_.size() > 1)
        label += i18n::Tr("busy.count", {{L"i", std::to_wstring(i + 1)}, {L"n", std::to_wstring(importJobs_.size())}});
    return label;
}

void Host::FinishImport() {
    if (worker_.joinable()) worker_.join();
    importing_ = false;
    Busy(L"", 0);
    std::vector<staging::Result> results = std::move(importResults_);
    results.resize(importJobs_.size());
    bool edited = false;
    if (cancel_.load()) {
        // Stopped from the busy bar: nothing staged so far is installed.
        Toast(i18n::Tr("msg.import_cancelled"));
    } else if (importWallpaper_) {
        const staging::Result &r = results[0];
        if (r.ok) {
            const std::wstring id = NewWallpaperId();
            const int code = commit::RunElevated(hwnd_, L"--commit-wallpaper " + id + L" " + commit::Quote(importJobs_[0].dir));
            if (code == commit::kOk) {
                library_.Reload();
                pictures_.Forget();
                // Brought in from a theme's page, for that theme.
                edited = !library_.open().empty() && library_.SetWallpaper(id);
                Toast(i18n::Tr("msg.imported_wallpaper", {{L"name", r.name}}));
            } else if (code == commit::kDeclined) {
                Toast(i18n::Tr("msg.import_cancelled"));
            } else {
                Problem(i18n::Tr("msg.import_write.wallpaper", {{L"log", paths::LogPath(L"config.log")}}));
            }
        } else {
            Problem(r.error.empty() ? i18n::Tr("msg.import_failed") : r.error);
        }
    } else {
        std::vector<size_t> staged;
        std::wstring problems;  // what could not be staged, a line each
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i].ok) {
                staged.push_back(i);
                continue;
            }
            const std::wstring why = results[i].error.empty() ? i18n::Tr("msg.import_failed") : results[i].error;
            // A Workshop item's card says it too, until it is tried again.
            if (!importJobs_[i].item.empty() && !cancel_.load()) steamProblems_[importJobs_[i].item] = why;
            if (results.size() == 1) problems = why;
            else {
                const ImportJob &j = importJobs_[i];
                problems += L"\n" + i18n::Tr("msg.not_imported.item", {{L"name", j.name.empty() ? FileStem(j.source) : j.name}, {L"why", why}});
            }
        }
        if (!staged.empty()) {
            const std::set<std::wstring> before = InstalledThemeIds();
            // All of them on one prompt, unless they are more than one command line holds. An
            // update goes in together with taking away the theme it replaces, on the same line.
            std::vector<std::wstring> groups;
            const steam_workshop::Reuse reuse = StagedReuse(staged);
            for (size_t k = 0; k < staged.size(); ++k) {
                const ImportJob &j = importJobs_[staged[k]];
                groups.push_back(L"--commit-package " + commit::Quote(j.dir) + ReplaceCommands(j, reuse));
            }
            int code = commit::kOk;
            std::wstring args;
            size_t commands = 0, runs = 0;
            for (size_t k = 0; k < groups.size() && code == commit::kOk; ++k) {
                args += (args.empty() ? L"" : L" ") + groups[k];
                ++commands;
                const bool last = k + 1 == groups.size();
                if (last || args.size() + 1 + groups[k + 1].size() > kMaxElevatedArgs) {
                    ALOG(L"import: %zu package(s) on elevated command line %zu", commands, ++runs);
                    code = commit::RunElevated(hwnd_, args);
                    args.clear();
                    commands = 0;
                }
            }
            // The new themes are the entries that were not there before.
            std::vector<std::wstring> added;
            for (const std::wstring &id : InstalledThemeIds())
                if (!before.count(id)) added.push_back(id);
            library_.Reload();
            pictures_.Forget();
            const bool fromSteam = !importJobs_[staged[0]].item.empty();
            const int updates = fromSteam ? RecordSteamInstalls(staged, added, results) : 0;
            if (fromSteam) {
                // Brought in from the Workshop, many or one: what is shown stays as it is, but for an
                // update of a theme in use, which has taken its place.
                if (added.size() == 1) {
                    const ThemeEntry *e = library_.Entry(added[0]);
                    Toast(i18n::Tr(updates ? "msg.steam.updated" : "msg.steam.installed", {{L"name", e ? e->name : results[staged[0]].name}}));
                } else if (!added.empty())
                    Toast(i18n::Tr("msg.imported_many", {{L"n", std::to_wstring(added.size())}}));
            } else if (importJobs_.size() == 1) {
                // What was just imported is what the person wants to see next.
                if (code == commit::kOk) {
                    const bool applied = added.size() == 1 && library_.Apply(added[0], L"all");
                    if (applied) Save();
                    Toast(i18n::Tr(applied ? "msg.imported_applied" : "msg.imported", {{L"name", results[staged[0]].name}}));
                }
            } else if (!added.empty()) {
                Toast(i18n::Tr("msg.imported_many", {{L"n", std::to_wstring(added.size())}}));
            }
            if (code == commit::kDeclined) {
                if (added.empty()) Toast(i18n::Tr("msg.import_cancelled"));
            } else if (code != commit::kOk) {
                Problem(i18n::Tr("msg.import_write.theme", {{L"log", paths::LogPath(L"config.log")}}));
            }
        }
        if (!problems.empty())
            Problem(results.size() == 1 ? problems : i18n::Tr("msg.not_imported", {{L"n", std::to_wstring(results.size() - staged.size())}}) + problems);
    }
    for (const ImportJob &j : importJobs_) secure::RemoveTree(j.dir);  // best effort; each holds only its own staging
    importJobs_.clear();
    if (steam_ && !steamShown_.empty()) SendSteamItems();
    if (edited) Edited(true, false, 0);
    else SendState();
}

void Host::FindWorkshop() {
    if (scanning_) return;
    scanning_ = true;
    scanner_ = std::thread([this, folders = workshopFolders_] {
        scanned_ = workshop::Scan(folders);
        PostMessageW(hwnd_, kWorkshopFound, 0, 0);
    });
}

void Host::FoundWorkshop() {
    if (scanner_.joinable()) scanner_.join();
    scanning_ = false;
    workshop_ = std::move(scanned_);
    json::Writer w;
    w.BeginObject().Member(L"type", L"workshop").Member(L"folders", (int)workshop_.folders.size());
    w.Member(L"others", workshop_.others).Key(L"items").BeginArray();
    for (const workshop::Item &v : workshop_.videos) {
        w.BeginObject().Member(L"id", v.id).Member(L"title", v.title);
        w.Member(L"base", L"https://" + WorkshopHost(v.folder) + L"/").Member(L"dir", v.dir);
        w.Member(L"video", v.video).Member(L"preview", v.preview).Member(L"bytes", (double)v.bytes);
        w.Member(L"source", workshop_.folders[v.folder].source == workshop::Source::Mine ? L"mine" : L"workshop");
        w.EndObject();
    }
    w.EndArray().EndObject();
    Send(w);
}

void Host::ImportWorkshop(const json::Value &ids, bool wallpaper) {
    // Only what the last look found: the page names wallpapers, never paths.
    std::vector<ImportJob> jobs;
    for (const json::Value &id : ids.Items()) {
        for (const workshop::Item &v : workshop_.videos)
            if (v.id == id.Str()) {
                jobs.push_back({workshop_.folders[v.folder].path + L"\\" + v.dir + L"\\" + v.video, v.title, L""});
                break;
            }
        if (wallpaper && !jobs.empty()) break;
    }
    if (jobs.empty()) {
        Problem(i18n::Tr("msg.workshop_missing"));
        return;
    }
    StartImport(std::move(jobs), wallpaper);
}

void Host::ImportComponent(bool add) {
    if (RefuseWhileImporting()) return;
    const std::wstring path = PickFile(hwnd_, Filters({{i18n::Tr("filter.components"), L"*.xml"}, {i18n::Tr("filter.all"), L"*.*"}}).c_str(), L"xml", false, L"");
    if (path.empty()) return;
    const std::wstring dir = staging::NewImportDir();
    if (dir.empty()) {
        Problem(i18n::Tr("msg.temp_dir", {{L"path", paths::UserDataDir() + L"\\import"}}));
        return;
    }
    const staging::Result r = staging::Component(path, dir);
    if (!r.ok) {
        secure::RemoveTree(dir);
        Problem(i18n::Tr("msg.component_invalid", {{L"why", r.error}}));
        return;
    }
    const std::wstring id = NewComponentId();
    const int code = commit::RunElevated(hwnd_, L"--commit-component " + id + L" " + commit::Quote(dir));
    secure::RemoveTree(dir);
    if (code == commit::kOk) {
        library_.Reload();
        Toast(i18n::Tr("msg.imported_component", {{L"name", r.name}}));
        if (add && !library_.open().empty()) {
            const std::wstring instance = library_.AddInstance(id);
            if (!instance.empty()) {
                json::Writer w;
                w.BeginObject().Member(L"type", L"added").Member(L"instance", instance).EndObject();
                Send(w);
            }
            Edited(!instance.empty(), false, 0);
            return;
        }
    } else if (code == commit::kDeclined) {
        Toast(i18n::Tr("msg.import_cancelled"));
    } else {
        Problem(i18n::Tr("msg.import_write.component"));
    }
    SendState();
}

std::wstring Host::CommitTheme(const theme::Theme &t, const std::wstring &then) {
    // Written and read back as the elevated helper will read it.
    const std::string text = theme::Normalize(t);
    theme::Theme check;
    std::wstring error;
    if (!theme::Parse(text, theme::Form::Installed, &check, &error)) {
        Problem(i18n::Tr("msg.save_theme_failed", {{L"why", error}}));
        return L"";
    }
    const std::wstring dir = staging::NewImportDir();
    if (dir.empty() || !WriteBytes(dir + L"\\theme.xml", text)) {
        if (!dir.empty()) secure::RemoveTree(dir);
        Problem(i18n::Tr("msg.temp_dir", {{L"path", paths::UserDataDir() + L"\\import"}}));
        return L"";
    }
    const std::wstring id = NewThemeId();
    const int code = commit::RunElevated(hwnd_, L"--commit-theme " + id + L" " + commit::Quote(dir) + then);
    secure::RemoveTree(dir);
    if (code == commit::kDeclined) Toast(i18n::Tr("msg.cancelled"));
    else if (code != commit::kOk && !ThemeExists(id)) Problem(i18n::Tr("msg.write_failed"));
    return ThemeExists(id) ? id : L"";
}

void Host::NewTheme(const std::wstring &name, const std::wstring &wallpaper) {
    theme::Theme t;
    t.name = name;
    t.wallpaper = IsWallpaperRef(wallpaper) ? wallpaper : std::wstring(kDefaultWallpaper);
    t.components.push_back({kClockComponent, kClockComponent, true, {}});
    const std::wstring id = CommitTheme(t);
    if (id.empty()) return;
    library_.Reload();
    library_.Open(id);
    Toast(i18n::Tr("msg.created", {{L"name", name}}));
    json::Writer w;
    w.BeginObject().Member(L"type", L"navigate").Member(L"theme", id).EndObject();
    Send(w);
    RenderPreview();
    SendState();
}

void Host::CopyTheme(const std::wstring &id, const std::wstring &name) {
    if (!library_.Entry(id)) return;
    // The copy is the theme as it shows now, with its edits made part of it.
    theme::Theme t = library_.Edited(id);
    t.name = name;
    if (CommitTheme(t).empty()) return;
    library_.Reload();
    Toast(i18n::Tr("msg.created", {{L"name", name}}));
    SendState();
}

void Host::RenameTheme(const std::wstring &id, const std::wstring &name) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e || e->builtIn) return;
    // A theme's name is in its protected theme.xml, and installed entries are never written
    // over: the theme goes in again under a new id, and the old entry goes, on one prompt.
    theme::Theme t = library_.Own(id);
    t.name = name;
    const std::wstring added = CommitTheme(t, L" --remove-theme " + id);
    if (added.empty()) return;
    if (!ThemeExists(id)) library_.Move(id, added);
    Save();
    library_.Reload();
    Toast(i18n::Tr("msg.renamed", {{L"name", name}}));
    if (library_.open() == added) RenderPreview();
    SendState();
    if (library_.open() == added) SendPreview(0);
}

void Host::RemoveTheme(const std::wstring &id) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e || e->builtIn) return;
    const std::wstring name = e->name;
    std::vector<std::wstring> wallpapers, components;
    library_.OnlyUsedBy(id, &wallpapers, &components);
    // One prompt for all of it; the theme goes first, so nothing is left naming what goes after.
    std::wstring args = L"--remove-theme " + id;
    for (const std::wstring &w : wallpapers) args += L" --remove-wallpaper " + w;
    for (const std::wstring &c : components) args += L" --remove-component " + c;
    const int code = commit::RunElevated(hwnd_, args);
    const bool gone = code != commit::kDeclined && !ThemeExists(id);
    if (gone) {
        library_.Forget(id);
        Save();
    }
    library_.Reload();
    pictures_.Forget();
    if (code == commit::kOk) Toast(i18n::Tr("msg.deleted", {{L"name", name}}));
    else if (code == commit::kDeclined) Toast(i18n::Tr("msg.cancelled"));
    else if (gone) Problem(i18n::Tr("msg.deleted_partly", {{L"name", name}}));
    else Problem(i18n::Tr("msg.delete_failed"));
    SendState();
    // A theme from the Workshop that goes is no longer installed there.
    if (gone && steam_ && !steamShown_.empty()) SendSteamItems();
}

void Host::ExportTheme(const std::wstring &id) {
    const ThemeEntry *e = library_.Entry(id);
    if (!e) return;
    const std::wstring path = PickFile(hwnd_, Filters({{i18n::Tr("filter.theme_export"), L"*.altheme"}, {i18n::Tr("filter.all"), L"*.*"}}).c_str(), L"altheme",
                                       true, FileNameFor(e->name) + L".altheme");
    if (path.empty()) return;
    HCURSOR was = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring error;
    const bool ok = exporter::Export(id, library_.settings(), path, &error);
    SetCursor(was);
    if (ok) Toast(i18n::Tr("msg.exported", {{L"path", path}}));
    else Problem(error);
}

void Host::Switch(bool on) {
    if (!paths::Sandbox().empty()) {
        Problem(i18n::Tr("msg.sandbox.switch"));
        SendEnvironment();
        return;
    }
    // Switching on switches these off (others.h).
    std::wstring stopping;
    for (const others::Program &p : others::Installed())
        if (on && p.on) stopping += (stopping.empty() ? L"" : i18n::Tr("list.join")) + p.name;
    const int code = commit::RunElevated(hwnd_, on ? L"--switch-on" : L"--switch-off");
    if (code == commit::kOk && !on) Toast(i18n::Tr("msg.off"));
    else if (code == commit::kOk) Toast(stopping.empty() ? i18n::Tr("msg.on") : i18n::Tr("msg.on_others", {{L"names", stopping}}));
    else if (code == commit::kDeclined) Toast(i18n::Tr("msg.cancelled"));
    else Problem(i18n::Tr("msg.switch_failed"));
    SendEnvironment();
}

}  // namespace app
