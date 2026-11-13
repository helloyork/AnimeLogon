#include "frame.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace app {

namespace {

constexpr wchar_t kEdgeClass[] = L"AnimeLogonAppEdge";

// The page's greys (app/web/src/styles/tokens.css), light and dark: its background, its bar
// (--surface-1), and a border as strong as the page's control outlines (--border-strong over the
// bar). They stand in for Windows' caption and border colours, which would otherwise be the
// person's accent colour when "Show accent colour on title bars and window borders" is on.
constexpr COLORREF kBackground[2] = {RGB(0xF4, 0xF4, 0xF6), RGB(0x11, 0x12, 0x14)};
constexpr COLORREF kBar[2] = {RGB(0xFA, 0xFA, 0xFB), RGB(0x17, 0x18, 0x1B)};
constexpr COLORREF kBorder[2] = {RGB(0xD5, 0xD5, 0xD5), RGB(0x38, 0x38, 0x3B)};

// How much of a maximised window's edge is left to a taskbar that hides itself on that edge, so
// that touching the edge still brings the taskbar back.
constexpr int kAutoHideTaskbarRoom = 2;

}  // namespace

COLORREF Frame::Background(bool dark) { return kBackground[dark ? 1 : 0]; }

void Frame::Attach(HWND window, bool dark) {
    window_ = window;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = EdgeProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kEdgeClass;
    RegisterClassExW(&wc);
    // Layered without a surface of their own, the edges draw nothing and show the page under them,
    // but still take the pointer where they are.
    for (HWND *edge : {&top_, &max_}) {
        *edge = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP, kEdgeClass, L"", WS_CHILD, 0, 0, 0, 0,
                                window_, nullptr, instance, this);
        if (*edge) SetLayeredWindowAttributes(*edge, 0, 255, LWA_ALPHA);
    }
    Colours(dark);
    // The window was made with its caption; WM_NCCALCSIZE takes it away from now on.
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Frame::Colours(bool dark) {
    dark_ = dark;
    const int i = dark ? 1 : 0;
    // Dark mode first: Windows draws the rest of the frame (the system menu, the border while the
    // window is inactive on Windows 10) to match it.
    const BOOL immersiveDark = dark;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersiveDark, sizeof(immersiveDark));
    // Windows 11 only; Windows 10 refuses them and keeps its own greys.
    DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &kBar[i], sizeof(COLORREF));
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &kBorder[i], sizeof(COLORREF));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    // What shows where the page has not drawn yet, such as the part a resize has just uncovered.
    if (HBRUSH brush = CreateSolidBrush(kBackground[i])) {
        if (auto *old = reinterpret_cast<HBRUSH>(SetClassLongPtrW(window_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush))))
            DeleteObject(old);
        InvalidateRect(window_, nullptr, TRUE);
    }
}

int Frame::Border() const {
    const UINT dpi = GetDpiForWindow(window_);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

bool Frame::Handle(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    // Until Attach, while the window is being created, it keeps Windows' frame.
    if (!window_) return false;
    switch (msg) {
    case WM_NCCALCSIZE: {
        if (!wp) return false;
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
        const RECT proposed = params->rgrc[0];
        *result = DefWindowProcW(window_, msg, wp, lp);
        RECT &client = params->rgrc[0];
        // Windows took off the borders and the caption: the client area gets the caption back, so
        // the page reaches the top of the window. The side and bottom borders stay outside it,
        // where Windows resizes the window from them and draws the shadow.
        if (!IsZoomed(window_)) {
            client.top = proposed.top;
            return true;
        }
        // A maximised window hangs over its monitor by its border on every side; the client area
        // stops short of the top by as much as Windows left at the sides.
        client.top = proposed.top + (client.left - proposed.left);
        // A taskbar that hides itself needs a sliver of its edge left uncovered to come back.
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EqualRect(&monitor.rcMonitor, &monitor.rcWork)) {
            for (UINT edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM}) {
                APPBARDATA bar{sizeof(bar)};
                bar.uEdge = edge;
                bar.rc = monitor.rcMonitor;
                if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) continue;
                if (edge == ABE_LEFT) client.left += kAutoHideTaskbarRoom;
                if (edge == ABE_TOP) client.top += kAutoHideTaskbarRoom;
                if (edge == ABE_RIGHT) client.right -= kAutoHideTaskbarRoom;
                if (edge == ABE_BOTTOM) client.bottom -= kAutoHideTaskbarRoom;
            }
        }
        return true;
    }
    case WM_NCHITTEST: {
        // Reached where the page does not cover the window: the borders outside the client area,
        // which Windows answers, and the client area while the page is not there yet.
        *result = DefWindowProcW(window_, msg, wp, lp);
        if (*result == HTCLIENT && !IsZoomed(window_)) {
            POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(window_, &at);
            if (at.y < Border()) *result = HTTOP;
        }
        return true;
    }
    case WM_SIZE:
        Place();
        if (maximised_ != !!IsZoomed(window_)) {
            maximised_ = !!IsZoomed(window_);
            Tell();
        }
        return false;
    case WM_NCACTIVATE:
        // Whether Windows would draw the caption as active: not while another window, or a dialog
        // of this one, is.
        if (active_ != !!wp) {
            active_ = !!wp;
            if (!active_) hover_ = press_ = false;
            Tell();
        }
        return false;
    }
    return false;
}

void Frame::Raise() { Place(); }

void Frame::Place() {
    if (!top_ || !max_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const bool edge = !IsZoomed(window_) && !IsIconic(window_);
    const int border = Border();
    const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (edge) SetWindowPos(top_, HWND_TOP, 0, 0, client.right, border, flags | SWP_SHOWWINDOW);
    else ShowWindow(top_, SW_HIDE);
    // The page keeps its buttons at the right end of the bar: measured from the right edge, the
    // button is where it belongs even before the page has laid itself out at a new size.
    RECT button{client.right - maxButton_.left, maxButton_.top, client.right - maxButton_.right, maxButton_.bottom};
    if (edge) button.top = std::max<LONG>(button.top, border);
    if (!IsIconic(window_) && button.right > button.left && button.bottom > button.top)
        SetWindowPos(max_, HWND_TOP, button.left, button.top, button.right - button.left, button.bottom - button.top,
                     flags | SWP_SHOWWINDOW);
    else ShowWindow(max_, SW_HIDE);
}

void Frame::Tell() {
    wchar_t title[256] = L"";
    GetWindowTextW(window_, title, ARRAYSIZE(title));
    json::Writer w;
    w.BeginObject().Member(L"type", L"frame").Member(L"title", title);
    w.Member(L"maximised", !!IsZoomed(window_)).Member(L"active", active_);
    w.Member(L"max", press_ ? L"press" : hover_ ? L"hover" : L"").EndObject();
    send_(w);
}

bool Frame::OnPage(const json::Value &m) {
    const std::wstring op = m[L"op"].Str();
    if (op == L"ready") {
        // The page has just (re)started: it learns how the window is, then the window answers
        // `ready` as usual.
        Tell();
        return false;
    }
    if (op == L"frame") {
        // Where the page drew its maximise button, in device pixels from the top left of the page,
        // which covers the client area. Null when the page has no such button.
        const json::Value &r = m[L"max"];
        const auto px = [&](const wchar_t *key) { return (LONG)std::lround(std::clamp(r[key].Num(), 0.0, 32767.0)); };
        RECT client{};
        GetClientRect(window_, &client);
        maxButton_ = {};
        if (r.IsObject() && px(L"right") > px(L"left") && px(L"bottom") > px(L"top"))
            maxButton_ = {client.right - px(L"left"), px(L"top"), client.right - px(L"right"), px(L"bottom")};
        Place();
        return true;
    }
    if (op == L"window") {
        // The page's buttons, as Windows' caption buttons do it: through the system menu's
        // commands, so the window animates and anything listening for them hears them.
        const std::wstring action = m[L"action"].Str();
        const WPARAM command = action == L"minimise" ? SC_MINIMIZE
                               : action == L"maximise" ? SC_MAXIMIZE
                               : action == L"restore"  ? SC_RESTORE
                               : action == L"close"    ? SC_CLOSE
                                                       : 0;
        if (command) PostMessageW(window_, WM_SYSCOMMAND, command, 0);
        return true;
    }
    return false;
}

LRESULT CALLBACK Frame::EdgeProc(HWND edge, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(edge, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    auto *self = reinterpret_cast<Frame *>(GetWindowLongPtrW(edge, GWLP_USERDATA));
    return self ? self->Edge(edge, msg, wp, lp) : DefWindowProcW(edge, msg, wp, lp);
}

LRESULT Frame::Edge(HWND edge, UINT msg, WPARAM wp, LPARAM lp) {
    if (edge == top_) {
        switch (msg) {
        case WM_NCHITTEST: {
            // The top edge resizes up, and both ways near the corners, as far along as the
            // window's side borders reach down.
            RECT r{};
            GetWindowRect(edge, &r);
            const int corner = MulDiv(16, GetDpiForWindow(window_), 96), x = GET_X_LPARAM(lp);
            return x < r.left + corner ? HTTOPLEFT : x >= r.right - corner ? HTTOPRIGHT : HTTOP;
        }
        // The window resizes itself from the hit the edge gave it, and double-clicking the top
        // edge stretches it to the monitor's height, as Windows does with its own border.
        case WM_NCMOUSEMOVE:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP:
        case WM_NCLBUTTONDBLCLK:
            return SendMessageW(window_, msg, wp, lp);
        // A right click on the edge is a right click on the bar: the system menu.
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
        case WM_NCRBUTTONDBLCLK:
            return SendMessageW(window_, msg, HTCAPTION, lp);
        }
        return DefWindowProcW(edge, msg, wp, lp);
    }

    // The maximise button. The page draws it; this takes the pointer over it so that Windows sees
    // a maximise button there and offers Snap Layouts, and tells the page how to draw it.
    switch (msg) {
    case WM_NCHITTEST:
        return HTMAXBUTTON;
    case WM_NCMOUSEMOVE:
        if (!hover_) {
            hover_ = true;
            Tell();
        }
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, edge, HOVER_DEFAULT};
            tracking_ = !!TrackMouseEvent(&track);
        }
        break;
    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ || press_) {
            hover_ = press_ = false;
            Tell();
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        // Not to Windows, which would draw its own button over the page's.
        press_ = true;
        Tell();
        return 0;
    case WM_NCLBUTTONUP:
        if (press_) {
            press_ = false;
            Tell();
            PostMessageW(window_, WM_SYSCOMMAND, IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE, 0);
        }
        return 0;
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCRBUTTONDBLCLK:
        return 0;
    }
    return DefWindowProcW(edge, msg, wp, lp);
}

}  // namespace app
