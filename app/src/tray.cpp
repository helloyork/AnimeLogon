#include "tray.h"

#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <vector>

#include "animelogon/i18n.h"
#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/version.h"

#include "library.h"
#include "traymenu.h"

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace app {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kNotify = WM_APP + 1;  // what happens to the icon
// At sign-in the program may start before the taskbar is there to take the icon: it tries again.
constexpr UINT_PTR kRetryTimer = 1;
constexpr UINT kRetryMs = 2000, kRetries = 60;
constexpr ULONGLONG kTipMs = 2000;  // the tooltip is worked out again at most this often
constexpr size_t kNameChars = 48;   // longer theme names are cut short in the menu

// The menu's commands. A theme's is kTheme plus its place among `choices_`.
enum : UINT { kOpen = 1, kLock, kSwitch, kExit, kTheme = 100 };

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (n == 0) return {};
        if (n < path.size()) return path.resize(n), path;
        path.resize(path.size() * 2);
    }
}

// What keeps a sandbox's programs in the sandbox, for their command lines.
std::wstring SandboxArgs() {
    std::wstring dir = paths::Sandbox();
    if (dir.empty()) return {};
    if (dir.back() == L'\\') dir += L'\\';  // a drive's root: the closing quote must not be escaped
    return L" --sandbox \"" + dir + L"\"";
}

// This program again, as this process's user, with `args`.
bool Start(const std::wstring &args) {
    std::wstring cmd = L"\"" + ModulePath() + L"\"" + SandboxArgs() + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ALOG(L"cannot start %s (%lu)", cmd.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// '&' would mark the next letter as the item's access key; long names are cut.
std::wstring MenuText(std::wstring text) {
    if (text.size() > kNameChars) text = text.substr(0, kNameChars - 1) + L"…";
    std::wstring out;
    for (wchar_t c : text) out += c == L'&' ? L"&&" : std::wstring(1, c);
    return out;
}

// Windows draws a program's popup menus light unless it opts in, through uxtheme functions
// exported only by ordinal since Windows 10 1903 (135 SetPreferredAppMode, 136 FlushMenuThemes),
// as Windows' own programs do. The menu follows "Choose your Windows mode", as the taskbar's do;
// with high contrast on, Windows' colours are left alone.
void MenusLikeTheTaskbar() {
    static const bool supported = [] {
        using GetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
        const auto get = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW v{sizeof(v)};
        return get && get(&v) == 0 && (v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 18362));
    }();
    static const HMODULE ux = supported ? LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
    if (!ux) return;
    using SetPreferredAppMode = int(WINAPI *)(int);
    using FlushMenuThemes = void(WINAPI *)();
    const auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    const auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (!set || !flush) return;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool high = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                      (contrast.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    enum { Default = 0, ForceDark = 2, ForceLight = 3 };
    set(high ? Default : light ? ForceLight : ForceDark);
    flush();
}

// The icon as a 32-bit bitmap with premultiplied alpha, which is what a menu item draws with its
// transparency.
HBITMAP BitmapOf(HICON icon) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    if (!icon || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmapFromHICON(icon, &bitmap)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || !w || !h) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;  // top-down, as WIC copies the rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE *>(bits)))) {
        DeleteObject(dib);
        dib = nullptr;
    }
    return dib;
}

class Tray {
public:
    ~Tray() {
        if (icon_) DestroyIcon(icon_);
        if (shield_) DeleteObject(shield_);
    }
    bool Create();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    NOTIFYICONDATAW Data(UINT flags) const;
    bool Add();
    void LoadIcons();
    void Tip(bool now);
    void ShowMenu(POINT at);
    void Do(UINT command);
    void Put(const TrayTheme &choice);
    void Warn(const std::wstring &title, const std::wstring &text);

    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    HICON icon_ = nullptr;
    HBITMAP shield_ = nullptr;
    bool added_ = false;
    UINT retries_ = 0;
    ULONGLONG tipAt_ = 0;
    std::wstring tip_;
    std::vector<TrayTheme> choices_;  // the themes of the menu last shown
};

bool Tray::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) return false;
    // Never shown: it owns the icon and the menu, and Explorer's TaskbarCreated reaches it.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, WindowTitle().c_str(), WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            wc.hInstance, this);
    if (!hwnd_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
    LoadIcons();
    Tip(true);
    if (!Add()) {
        ALOG(L"tray: no taskbar yet -- trying again");
        SetTimer(hwnd_, kRetryTimer, kRetryMs, nullptr);
    }
    return true;
}

LRESULT CALLBACK Tray::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto *self = static_cast<Tray *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto *self = reinterpret_cast<Tray *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Tray::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_) {
        // Explorer started again (or the display's scale changed): the icon is put back.
        added_ = false;
        LoadIcons();
        if (!Add()) SetTimer(hwnd_, kRetryTimer, kRetryMs, nullptr);
        return 0;
    }
    switch (msg) {
    case kNotify:
        // NOTIFYICON_VERSION_4: the event is in lParam's low word, where it happened in wParam.
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: ShowApp(L""); break;
        case WM_CONTEXTMENU: ShowMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}); break;
        case WM_MOUSEMOVE: Tip(false); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kRetryTimer && (Add() || ++retries_ >= kRetries)) {
            KillTimer(hwnd_, kRetryTimer);
            if (!added_) ALOG(L"tray: the taskbar never took the icon");
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(hwnd_);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid = Data(0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

NOTIFYICONDATAW Tray::Data(UINT flags) const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = flags;
    nid.uCallbackMessage = kNotify;
    nid.hIcon = icon_;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    return nid;
}

bool Tray::Add() {
    NOTIFYICONDATAW nid = Data(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    // An icon left over from before Explorer's restart is changed rather than added twice.
    if (!Shell_NotifyIconW(NIM_ADD, &nid) && !Shell_NotifyIconW(NIM_MODIFY, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    if (!added_) ALOG(L"tray: the icon is in the notification area");
    added_ = true;
    return true;
}

// The program's icon at the notification area's size, and the shield that marks what needs an
// administrator, both for the main display's scale.
void Tray::LoadIcons() {
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (icon_) DestroyIcon(icon_);
    icon_ = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(ANIMELOGON_ICON_ID), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (shield_) DeleteObject(shield_);
    shield_ = nullptr;
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
        shield_ = BitmapOf(sii.hIcon);
        DestroyIcon(sii.hIcon);
    }
}

// Two lines: AnimeLogon (or the sandbox), said to be off when it is; and what the logon screen shows.
void Tray::Tip(bool now) {
    const ULONGLONG tick = GetTickCount64();
    if (!now && tick - tipAt_ < kTipMs) return;
    tipAt_ = tick;
    std::wstring text = WindowTitle();
    if (paths::Sandbox().empty() && !machine::InstallDir().empty() && !machine::IsOn()) text += i18n::Tr("tray.off");
    const Library library(DiskStores());
    const std::wstring showing = ShowingNow(library, EnumerateMonitors());
    if (!showing.empty()) text += L"\n" + showing;
    if (text == tip_) return;
    tip_ = text;
    if (!added_) return;
    NOTIFYICONDATAW nid = Data(NIF_TIP | NIF_SHOWTIP);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::ShowMenu(POINT at) {
    // Read afresh: the settings app, or another account, may have changed what is in use.
    const Library library(DiskStores());
    const TrayMenu model = MenuFor(library, EnumerateMonitors());
    choices_.clear();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    auto item = [](HMENU to, UINT id, const std::wstring &text, UINT type, bool checked, HBITMAP bitmap) {
        std::wstring label = text;
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_STRING | (bitmap ? MIIM_BITMAP : 0);
        mi.fType = MFT_STRING | type;
        mi.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
        mi.wID = id;
        mi.dwTypeData = label.data();
        mi.hbmpItem = bitmap;
        InsertMenuItemW(to, (UINT)GetMenuItemCount(to), TRUE, &mi);
    };
    auto themes = [&](HMENU to, const std::vector<TrayTheme> &list) {
        for (const TrayTheme &t : list) {
            item(to, kTheme + (UINT)choices_.size(), MenuText(t.name), MFT_RADIOCHECK, t.checked, nullptr);
            choices_.push_back(t);
        }
    };

    item(menu, kOpen, i18n::Tr("button.open_app"), 0, false, nullptr);
    SetMenuDefaultItem(menu, kOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (model.displays.empty()) {
        themes(menu, model.themes);
    } else {
        for (const TrayDisplay &d : model.displays) {
            HMENU sub = CreatePopupMenu();
            themes(sub, d.themes);
            AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), MenuText(d.label).c_str());
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(menu, kLock, i18n::Tr("theme.lock_preview"), 0, false, nullptr);
    // Turning AnimeLogon on or off needs an administrator: it opens the settings app at its switch,
    // which asks. Not offered where it is not installed.
    const bool sandbox = !paths::Sandbox().empty();
    if (sandbox || !machine::InstallDir().empty())
        item(menu, kSwitch, i18n::Tr("tray.switch"), 0, sandbox || machine::IsOn(), shield_);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(menu, kExit, i18n::Tr("tray.exit"), 0, false, nullptr);

    MenusLikeTheTaskbar();
    // The menu closes when clicked away from only if its window is in the foreground.
    SetForegroundWindow(hwnd_);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, at.x, at.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    Do(command);
}

void Tray::Do(UINT command) {
    switch (command) {
    case 0: return;  // closed without a choice
    case kOpen: ShowApp(L""); return;
    case kLock: ShowApp(L"lock"); return;
    case kSwitch: ShowApp(L"switch"); return;
    case kExit:
        ALOG(L"tray: closed from its menu");
        DestroyWindow(hwnd_);
        return;
    }
    if (command >= kTheme && command - kTheme < choices_.size()) Put(choices_[command - kTheme]);
}

// Puts the theme on the logon screen, as the settings app does, and tells the app if it is open.
void Tray::Put(const TrayTheme &choice) {
    Library library(DiskStores());
    if (!library.Apply(choice.id, choice.monitor) || !library.Save()) {
        ALOG(L"tray: %s could not be put on the logon screen", choice.id.c_str());
        Warn(i18n::Tr("tray.error.title"), i18n::Tr(library.Entry(choice.id) ? "tray.error.save" : "tray.error.gone"));
        return;
    }
    ALOG(L"tray: %s is on the logon screen%s%s", choice.id.c_str(), choice.monitor.empty() ? L"" : L" -- display ",
         choice.monitor.c_str());
    if (HWND app = FindAppWindow()) {
        COPYDATASTRUCT cd{kAskReread, 0, nullptr};
        DWORD_PTR answer = 0;
        SendMessageTimeoutW(app, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd_), reinterpret_cast<LPARAM>(&cd),
                            SMTO_ABORTIFHUNG, 2000, &answer);
    }
    Tip(true);
}

// A notification from the icon, for what went wrong.
void Tray::Warn(const std::wstring &title, const std::wstring &text) {
    if (!added_) return;
    NOTIFYICONDATAW nid = Data(NIF_INFO);
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace

std::wstring WindowTitle() { return paths::Sandbox().empty() ? std::wstring(L"AnimeLogon") : paths::SandboxTitle(); }

bool IsShowTarget(std::wstring_view what) { return what.empty() || what == L"switch" || what == L"lock"; }

HWND FindAppWindow() { return FindWindowW(kAppClass, WindowTitle().c_str()); }
HWND FindTrayWindow() { return FindWindowW(kTrayClass, WindowTitle().c_str()); }

bool AskApp(HWND app, const std::wstring &what) {
    DWORD pid = 0;
    GetWindowThreadProcessId(app, &pid);
    // The person just clicked: the app may take the foreground from whoever has it.
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cd{kAskShow, (DWORD)(what.size() * sizeof(wchar_t)), const_cast<wchar_t *>(what.c_str())};
    DWORD_PTR answer = 0;
    return SendMessageTimeoutW(app, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 3000, &answer) && answer;
}

bool ShowApp(const std::wstring &what) {
    if (HWND app = FindAppWindow(); app && AskApp(app, what)) return true;
    AllowSetForegroundWindow(ASFW_ANY);
    return Start(what.empty() ? std::wstring() : L" --show " + what);
}

void StartTray() {
    // An icon started from an elevated window would run elevated too; the Run entry starts it
    // as the person at the next sign-in.
    if (FindTrayWindow() || machine::IsElevated()) return;
    Start(L" --tray");
}

void CloseTray() {
    if (HWND tray = FindTrayWindow()) PostMessageW(tray, WM_CLOSE, 0, 0);
}

int RunTray() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int code = 0;
    {
        instance::Guard only;
        const std::wstring name = paths::Sandbox().empty() ? std::wstring(L"Local\\AnimeLogon.Tray") : paths::SandboxMutex() + L".Tray";
        if (only.Acquire(name) != instance::Result::AlreadyRunning) {
            if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\tray.log");
            if (!LoadSettings(false).tray) {
                ALOG(L"tray: settings.ini says tray = false");
            } else {
                Tray tray;
                if (tray.Create()) {
                    MSG msg;
                    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                } else {
                    ALOG(L"tray: no window (%lu)", GetLastError());
                    code = 1;
                }
            }
        }
    }
    CoUninitialize();
    return code;
}

}  // namespace app
