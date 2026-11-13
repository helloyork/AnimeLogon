#include "ui.h"

#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "animelogon/version.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

using Microsoft::WRL::ComPtr;

namespace wizard {

namespace {

// The window's size, and the measures everything on it is placed by (DIPs), after the settings
// app's (app/web/src/styles/tokens.css): a 4 px grid, 32 px controls, 6 px corners.
constexpr float kWidth = 640, kHeight = 480;
constexpr float kPad = 36, kFooter = 68, kButtonH = 32, kButtonMinW = 96, kButtonPadX = 20, kGap = 8;
constexpr float kTitleTop = 28, kRadius = 6;

D2D1_COLOR_F Hex(unsigned rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

// The settings app's colours (tokens.css), light and dark.
struct Palette {
    bool dark = false;
    D2D1_COLOR_F bg, footer, field, surface, hover, press, border, borderStrong, text1, text2, text3, accent,
        accentHover, accentPress, onAccent, danger, warn, track;
};

Palette MakePalette(bool dark) {
    Palette p;
    p.dark = dark;
    if (!dark) {
        p.bg = Hex(0xf4f4f6); p.footer = Hex(0xfafafb); p.field = Hex(0xffffff); p.surface = Hex(0xffffff);
        p.hover = Hex(0xeeeef1); p.press = Hex(0xe6e6ea); p.border = Hex(0x000000, .08f); p.borderStrong = Hex(0x000000, .15f);
        p.text1 = Hex(0x18181b); p.text2 = Hex(0x56585f); p.text3 = Hex(0x8a8c93);
        p.accent = Hex(0x2f6fe4); p.accentHover = Hex(0x3d7cf0); p.accentPress = Hex(0x2662cf); p.onAccent = Hex(0xffffff);
        p.danger = Hex(0xc4314b); p.warn = Hex(0xb7791f); p.track = Hex(0x000000, .16f);
    } else {
        p.bg = Hex(0x111214); p.footer = Hex(0x17181b); p.field = Hex(0x1f2024); p.surface = Hex(0x1c1d21);
        p.hover = Hex(0x26272c); p.press = Hex(0x2d2e34); p.border = Hex(0xffffff, .075f); p.borderStrong = Hex(0xffffff, .14f);
        p.text1 = Hex(0xececef); p.text2 = Hex(0xa2a5ac); p.text3 = Hex(0x6d7078);
        p.accent = Hex(0x4a8af4); p.accentHover = Hex(0x5b96f6); p.accentPress = Hex(0x3c7de6); p.onAccent = Hex(0xffffff);
        p.danger = Hex(0xf07167); p.warn = Hex(0xe5b246); p.track = Hex(0xffffff, .18f);
    }
    return p;
}

COLORREF ToColorRef(const D2D1_COLOR_F &c) {
    return RGB((int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
}

bool AppsUseDarkTheme() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

bool FontInstalled(IDWriteFactory *dw, const wchar_t *family) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

// Segoe Fluent Icons is Windows 11's; Windows 10 has the same glyphs at the same code points in
// Segoe MDL2 Assets.
constexpr wchar_t kGlyphCheck = 0xE73E, kGlyphChevron = 0xE70D, kGlyphError = 0xEA39, kGlyphWarning = 0xE7BA,
                  kGlyphInfo = 0xE946;

}  // namespace

/* --- what a page holds --- */

enum class Kind {
    Title, Subtitle, Para, Heading, Bullet, Caption, Gap, Check, Field, Progress, Row, Problem, Link,
    Button, StartButton, HeaderMenu
};

struct Builder::Item {
    Kind kind;
    std::wstring text, detail;
    Tone tone = Tone::Primary;
    Severity severity = Severity::Info;
    int id = 0;
    bool primary = false, enabled = true, checked = false, link = false;
    float gap = 0;
    double fraction = 0;
};

void Builder::Title(const std::wstring &text) { items_.push_back(std::make_shared<Item>(Item{Kind::Title, text})); }
void Builder::Subtitle(const std::wstring &text, Tone tone) {
    auto i = std::make_shared<Item>(Item{Kind::Subtitle, text});
    i->tone = tone;
    items_.push_back(i);
}
void Builder::HeaderMenu(int id, const std::wstring &label, const std::wstring &name) {
    auto i = std::make_shared<Item>(Item{Kind::HeaderMenu, label, name});
    i->id = id;
    items_.push_back(i);
}
void Builder::Para(const std::wstring &text, Tone tone) {
    auto i = std::make_shared<Item>(Item{Kind::Para, text});
    i->tone = tone;
    items_.push_back(i);
}
void Builder::Heading(const std::wstring &text) { items_.push_back(std::make_shared<Item>(Item{Kind::Heading, text})); }
void Builder::Bullet(const std::wstring &text) { items_.push_back(std::make_shared<Item>(Item{Kind::Bullet, text})); }
void Builder::Caption(const std::wstring &text, Tone tone) {
    auto i = std::make_shared<Item>(Item{Kind::Caption, text});
    i->tone = tone;
    items_.push_back(i);
}
void Builder::Gap(float dips) {
    auto i = std::make_shared<Item>(Item{Kind::Gap});
    i->gap = dips;
    items_.push_back(i);
}
void Builder::Check(int id, const std::wstring &label, const std::wstring &detail, bool checked, bool enabled) {
    auto i = std::make_shared<Item>(Item{Kind::Check, label, detail});
    i->id = id;
    i->checked = checked;
    i->enabled = enabled;
    items_.push_back(i);
}
void Builder::Field(const std::wstring &value, int buttonId, const std::wstring &buttonLabel) {
    auto i = std::make_shared<Item>(Item{Kind::Field, value, buttonLabel});
    i->id = buttonId;
    items_.push_back(i);
}
void Builder::Progress(double fraction) {
    auto i = std::make_shared<Item>(Item{Kind::Progress});
    i->fraction = std::clamp(fraction, 0.0, 1.0);
    items_.push_back(i);
}
void Builder::Row(const std::wstring &label, const std::wstring &value) {
    items_.push_back(std::make_shared<Item>(Item{Kind::Row, label, value}));
}
void Builder::Problem(Severity severity, const std::wstring &title, const std::wstring &detail) {
    auto i = std::make_shared<Item>(Item{Kind::Problem, title, detail});
    i->severity = severity;
    items_.push_back(i);
}
void Builder::Link(int id, const std::wstring &text) {
    auto i = std::make_shared<Item>(Item{Kind::Link, text});
    i->id = id;
    i->link = true;
    items_.push_back(i);
}
void Builder::Button(int id, const std::wstring &label, bool primary, bool enabled) {
    auto i = std::make_shared<Item>(Item{Kind::Button, label});
    i->id = id;
    i->primary = primary;
    i->enabled = enabled;
    items_.push_back(i);
}
void Builder::StartButton(int id, const std::wstring &label, bool link) {
    auto i = std::make_shared<Item>(Item{Kind::StartButton, label});
    i->id = id;
    i->link = link;
    items_.push_back(i);
}

/* --- the window --- */

namespace {

enum class ControlKind { Primary, Secondary, Link, Menu, Check };

// A button as laid out: where (DIPs), what it says, how it is drawn.
struct Control {
    int id = 0;
    ControlKind kind = ControlKind::Secondary;
    D2D1_RECT_F rect{};
    std::wstring label, detail;
    bool enabled = true, checked = false;
};

// Something painted on the window itself.
struct Paint {
    enum class What { Text, Dot, FieldBox, Track, Icon, Hairline } what = What::Text;
    D2D1_RECT_F rect{};
    ComPtr<IDWriteTextLayout> layout;
    D2D1_COLOR_F color{};
    double fraction = 0;
    wchar_t glyph = 0;
};

}  // namespace

struct Window::Impl {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteTextFormat> title, body, bodyStrong, caption, button, icon, iconSmall;
    std::wstring family = L"Segoe UI", display = L"Segoe UI", iconFamily = L"Segoe MDL2 Assets", locale = L"en-US";
    Palette palette = MakePalette(false);
    UINT dpi = 96;
    std::vector<Paint> paints;
    std::vector<Control> controls;
    std::map<int, HWND> hwnds;
    int defaultId = 0;
    bool keyboardCues = false;

    float Px(float dip) const { return dip * dpi / 96.f; }

    void Fonts() {
        auto make = [&](const std::wstring &fam, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat> *out) {
            out->Reset();
            dw->CreateTextFormat(fam.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                 locale.c_str(), out->GetAddressOf());
            if (*out) (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        };
        make(display, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, &title);
        make(family, 14, DWRITE_FONT_WEIGHT_NORMAL, &body);
        make(family, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, &bodyStrong);
        make(family, 12, DWRITE_FONT_WEIGHT_NORMAL, &caption);
        make(family, 14, DWRITE_FONT_WEIGHT_NORMAL, &button);
        make(iconFamily, 16, DWRITE_FONT_WEIGHT_NORMAL, &icon);
        make(iconFamily, 12, DWRITE_FONT_WEIGHT_NORMAL, &iconSmall);
        if (button) {
            button->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        for (IDWriteTextFormat *f : {icon.Get(), iconSmall.Get()})
            if (f) {
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
    }

    ComPtr<IDWriteTextLayout> Layout(const std::wstring &text, IDWriteTextFormat *format, float width, float height = 10000) {
        ComPtr<IDWriteTextLayout> l;
        dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, std::max(width, 1.f), height, &l);
        return l;
    }
    static float Height(IDWriteTextLayout *l) {
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.height;
    }
    static float Width(IDWriteTextLayout *l) {
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
    float TextWidth(const std::wstring &text, IDWriteTextFormat *format) {
        ComPtr<IDWriteTextLayout> l;
        dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, 10000, 100, &l);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.width;
    }

    D2D1_COLOR_F ToneColor(Tone t) const {
        switch (t) {
        case Tone::Secondary: return palette.text2;
        case Tone::Tertiary: return palette.text3;
        case Tone::Danger: return palette.danger;
        case Tone::Warning: return palette.warn;
        default: return palette.text1;
        }
    }

    bool EnsureTarget() {
        if (target) return true;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            (float)dpi, (float)dpi);
        return SUCCEEDED(d2d->CreateDCRenderTarget(&props, &target));
    }
};

Window::Window() : impl_(std::make_unique<Impl>()) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(impl_->dw.GetAddressOf()));
    if (FontInstalled(impl_->dw.Get(), L"Segoe UI Variable Text")) impl_->family = L"Segoe UI Variable Text";
    if (FontInstalled(impl_->dw.Get(), L"Segoe UI Variable Display")) impl_->display = L"Segoe UI Variable Display";
    if (FontInstalled(impl_->dw.Get(), L"Segoe Fluent Icons")) impl_->iconFamily = L"Segoe Fluent Icons";
    impl_->palette = MakePalette(AppsUseDarkTheme());
    impl_->Fonts();
}

Window::~Window() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void Window::SetLocale(const std::wstring &locale) {
    impl_->locale = locale;
    impl_->Fonts();
}

namespace {

constexpr const wchar_t *kClass = L"AnimeLogonSetup";

// The caption Windows draws, in the page's colours: dark when the page is (Windows 10 2004 and
// later), and on Windows 11 the same colour as the page, so the window reads as one surface.
void StyleCaption(HWND hwnd, const Palette &p) {
    const BOOL dark = p.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = ToColorRef(p.bg), text = ToColorRef(p.text1);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}

}  // namespace

bool Window::Create(const std::wstring &title, const POINT *at) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HINSTANCE self = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = self;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(ANIMELOGON_ICON_ID), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(self, MAKEINTRESOURCEW(ANIMELOGON_ICON_ID), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // On the display under the pointer, centred in its work area, at that display's scale.
    POINT pt{};
    GetCursorPos(&pt);
    if (at) pt = *at;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_CONTROLPARENT;
    const int x0 = at ? at->x : mi.rcWork.left, y0 = at ? at->y : mi.rcWork.top;
    hwnd_ = CreateWindowExW(exStyle, kClass, title.c_str(), style, x0, y0, 100, 100, nullptr, nullptr, self, this);
    if (!hwnd_) return false;
    impl_->dpi = GetDpiForWindow(hwnd_);
    RECT r{0, 0, (LONG)std::lround(impl_->Px(kWidth)), (LONG)std::lround(impl_->Px(kHeight))};
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, impl_->dpi);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = at ? at->x : (mi.rcWork.left + mi.rcWork.right - w) / 2, y = at ? at->y : (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    StyleCaption(hwnd_, impl_->palette);
    Rebuild();
    return true;
}

void Window::SetDark(bool dark) {
    impl_->palette = MakePalette(dark);
    if (!hwnd_) return;
    StyleCaption(hwnd_, impl_->palette);
    Rebuild();
}

int Window::Run(const std::wstring &title) {
    if (!Create(title)) return 1;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (impl_->defaultId && impl_->hwnds.count(impl_->defaultId)) SetFocus(impl_->hwnds[impl_->defaultId]);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Enter presses the button that has the focus, as in a dialog, and the primary one
        // otherwise. The dialog manager would press the primary one even from 取消, since this is
        // not a dialog and keeps no default that follows the focus.
        if (hwnd_ && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd))) {
            HWND focus = GetFocus();
            const bool pushButton = focus && IsChild(hwnd_, focus) && (GetWindowLongW(focus, GWL_STYLE) & BS_TYPEMASK) == BS_PUSHBUTTON;
            if (pushButton) SendMessageW(focus, BM_CLICK, 0, 0);
            else if (impl_->defaultId) OnClick(impl_->defaultId);
            continue;
        }
        if (hwnd_ && IsDialogMessageW(hwnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return code_;
}

void Window::Close(int code) {
    code_ = code;
    if (hwnd_) DestroyWindow(hwnd_);
}

void Window::Post(UINT message, WPARAM w, LPARAM l) {
    if (hwnd_) PostMessageW(hwnd_, message, w, l);
}

bool Window::Checked(int id) const {
    auto i = impl_->hwnds.find(id);
    return i != impl_->hwnds.end() && SendMessageW(i->second, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

RECT Window::ScreenRect(int id) const {
    RECT r{};
    auto i = impl_->hwnds.find(id);
    if (i != impl_->hwnds.end()) GetWindowRect(i->second, &r);
    return r;
}

void Window::Rebuild() {
    if (!hwnd_) return;
    Impl &m = *impl_;
    Builder b(this);
    Build(b);

    m.paints.clear();
    m.controls.clear();
    m.defaultId = 0;
    const float right = kWidth - kPad, width = right - kPad;
    float y = kTitleTop;

    // The header: title, the menu beside it, the line under it.
    float menuW = 0;
    for (auto &it : b.items_)
        if (it->kind == Kind::HeaderMenu) {
            menuW = std::max(kButtonMinW, m.TextWidth(it->text, m.button.Get()) + kButtonPadX + 28);
            Control c{it->id, ControlKind::Menu, D2D1::RectF(right - menuW, y + 2, right, y + 2 + kButtonH), it->text, it->detail};
            m.controls.push_back(c);
        }
    for (auto &it : b.items_) {
        if (it->kind == Kind::Title) {
            auto l = m.Layout(it->text, m.title.Get(), width - (menuW ? menuW + 16 : 0));
            const float h = Impl::Height(l.Get());
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad, y, right, y + h), l, m.palette.text1});
            y += h + 4;
        } else if (it->kind == Kind::Subtitle) {
            auto l = m.Layout(it->text, m.body.Get(), width);
            const float h = Impl::Height(l.Get());
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad, y, right, y + h), l, m.ToneColor(it->tone)});
            y += h;
        }
    }
    y = std::max(y + 28, 112.f);

    // Rows have one label column, as wide as the widest label up to two fifths of the page.
    float labelW = 0;
    for (auto &it : b.items_)
        if (it->kind == Kind::Row) labelW = std::max(labelW, m.TextWidth(it->text, m.body.Get()));
    labelW = std::min(labelW + 24, width * 0.4f);

    // The body, top to bottom.
    for (auto &it : b.items_) {
        switch (it->kind) {
        case Kind::Para:
        case Kind::Caption: {
            auto l = m.Layout(it->text, it->kind == Kind::Para ? m.body.Get() : m.caption.Get(), width);
            const float h = Impl::Height(l.Get());
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad, y, right, y + h), l, m.ToneColor(it->tone)});
            y += h + (it->kind == Kind::Para ? 12 : 8);
            break;
        }
        case Kind::Heading: {
            auto l = m.Layout(it->text, m.bodyStrong.Get(), width);
            const float h = Impl::Height(l.Get());
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad, y, right, y + h), l, m.palette.text1});
            y += h + 8;
            break;
        }
        case Kind::Bullet: {
            auto l = m.Layout(it->text, m.body.Get(), width - 20);
            const float h = Impl::Height(l.Get());
            Paint dot{Paint::What::Dot, D2D1::RectF(kPad + 4, y + 8, kPad + 8, y + 12)};
            dot.color = m.palette.text3;
            m.paints.push_back(dot);
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad + 20, y, right, y + h), l, m.palette.text1});
            y += h + 8;
            break;
        }
        case Kind::Gap: y += it->gap; break;
        case Kind::Check: {
            const float textW = width - 32;
            const float h1 = Impl::Height(m.Layout(it->text, m.body.Get(), textW).Get());
            const float h2 = it->detail.empty() ? 0 : Impl::Height(m.Layout(it->detail, m.caption.Get(), textW).Get()) + 2;
            const float textRight = kPad + 32 + std::max(m.TextWidth(it->text, m.body.Get()),
                                                         it->detail.empty() ? 0.f : m.TextWidth(it->detail, m.caption.Get()));
            Control c{it->id, ControlKind::Check, D2D1::RectF(kPad, y, std::min(right, textRight + 4), y + std::max(20.f, h1 + h2)),
                      it->text, it->detail, it->enabled, it->checked};
            m.controls.push_back(c);
            y += std::max(20.f, h1 + h2) + 16;
            break;
        }
        case Kind::Field: {
            const float bw = std::max(kButtonMinW, m.TextWidth(it->detail, m.button.Get()) + 2 * kButtonPadX);
            const float boxRight = right - bw - kGap;
            auto l = m.Layout(it->text, m.body.Get(), boxRight - kPad - 24, kButtonH);
            if (l) {
                l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ellipsis;
                m.dw->CreateEllipsisTrimmingSign(m.body.Get(), &ellipsis);
                l->SetTrimming(&trim, ellipsis.Get());
            }
            Paint box{Paint::What::FieldBox, D2D1::RectF(kPad, y, boxRight, y + kButtonH), l, m.palette.text1};
            m.paints.push_back(box);
            m.controls.push_back({it->id, ControlKind::Secondary, D2D1::RectF(right - bw, y, right, y + kButtonH), it->detail});
            y += kButtonH + 8;
            break;
        }
        case Kind::Progress: {
            Paint p{Paint::What::Track, D2D1::RectF(kPad, y, right, y + 4)};
            p.fraction = it->fraction;
            m.paints.push_back(p);
            y += 4 + 12;
            break;
        }
        case Kind::Row: {
            auto l1 = m.Layout(it->text, m.body.Get(), labelW - 16);
            auto l2 = m.Layout(it->detail, m.body.Get(), width - labelW);
            const float h = std::max(Impl::Height(l1.Get()), Impl::Height(l2.Get()));
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad, y, kPad + labelW, y + h), l1, m.palette.text2});
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad + labelW, y, right, y + h), l2, m.palette.text1});
            y += h + 10;
            break;
        }
        case Kind::Problem: {
            auto l1 = m.Layout(it->text, m.bodyStrong.Get(), width - 28);
            auto l2 = it->detail.empty() ? nullptr : m.Layout(it->detail, m.body.Get(), width - 28);
            const float h1 = Impl::Height(l1.Get()), h2 = l2 ? Impl::Height(l2.Get()) : 0;
            Paint icon{Paint::What::Icon, D2D1::RectF(kPad, y + 1, kPad + 18, y + 19)};
            icon.glyph = it->severity == Severity::Error ? kGlyphError : it->severity == Severity::Warning ? kGlyphWarning : kGlyphInfo;
            icon.color = it->severity == Severity::Error ? m.palette.danger : it->severity == Severity::Warning ? m.palette.warn : m.palette.accent;
            m.paints.push_back(icon);
            m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad + 28, y, right, y + h1), l1, m.palette.text1});
            if (l2) m.paints.push_back({Paint::What::Text, D2D1::RectF(kPad + 28, y + h1 + 2, right, y + h1 + 2 + h2), l2, m.palette.text2});
            y += h1 + (l2 ? h2 + 2 : 0) + 14;
            break;
        }
        case Kind::Link: {
            const float w = m.TextWidth(it->text, m.body.Get()) + 4;
            m.controls.push_back({it->id, ControlKind::Link, D2D1::RectF(kPad - 2, y, kPad - 2 + w, y + 22), it->text});
            y += 22 + 8;
            break;
        }
        default: break;
        }
    }

    // The footer: its own band at the bottom, the buttons placed from the right.
    Paint line{Paint::What::Hairline, D2D1::RectF(0, kHeight - kFooter, kWidth, kHeight - kFooter + 1)};
    m.paints.push_back(line);
    const float by = kHeight - kFooter + (kFooter - kButtonH) / 2;
    float x = kWidth - 24;
    for (auto &it : b.items_) {
        if (it->kind == Kind::Button) {
            const float w = std::max(kButtonMinW, m.TextWidth(it->text, m.button.Get()) + 2 * kButtonPadX);
            m.controls.push_back({it->id, it->primary ? ControlKind::Primary : ControlKind::Secondary,
                                  D2D1::RectF(x - w, by, x, by + kButtonH), it->text, L"", it->enabled});
            if (it->primary && it->enabled) m.defaultId = it->id;
            x -= w + kGap;
        } else if (it->kind == Kind::StartButton) {
            const float w = it->link ? m.TextWidth(it->text, m.body.Get()) + 8
                                     : std::max(kButtonMinW, m.TextWidth(it->text, m.button.Get()) + 2 * kButtonPadX);
            const float top = it->link ? kHeight - kFooter + (kFooter - 22) / 2 : by;
            m.controls.push_back({it->id, it->link ? ControlKind::Link : ControlKind::Secondary,
                                  D2D1::RectF(24 - (it->link ? 4.f : 0.f), top, 24 + w, top + (it->link ? 22 : kButtonH)), it->text});
        }
    }

    // One button per id: kept, moved and relabelled where it was there before, so the focus stays
    // on it; the others go. Tab follows the page from the top left.
    std::stable_sort(m.controls.begin(), m.controls.end(), [](const Control &a, const Control &b) {
        if (std::fabs(a.rect.top - b.rect.top) > 1) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
    std::map<int, HWND> keep;
    HWND after = HWND_TOP;
    const HWND focused = GetFocus();
    bool focusLost = false;
    for (const Control &c : m.controls) {
        const bool check = c.kind == ControlKind::Check;
        HWND h = nullptr;
        auto old = m.hwnds.find(c.id);
        if (old != m.hwnds.end()) {
            const bool wasCheck = (GetWindowLongW(old->second, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX;
            if (wasCheck == check) h = old->second;
            else {
                if (old->second == focused) focusLost = true;
                DestroyWindow(old->second);
            }
            m.hwnds.erase(old);
        }
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (check ? BS_AUTOCHECKBOX | BS_MULTILINE : BS_PUSHBUTTON);
        if (!h) {
            h = CreateWindowExW(0, L"BUTTON", c.label.c_str(), style, 0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)c.id,
                                GetModuleHandleW(nullptr), nullptr);
        } else {
            wchar_t text[512] = {};
            GetWindowTextW(h, text, ARRAYSIZE(text));
            if (c.label != text) SetWindowTextW(h, c.label.c_str());
        }
        if (check) SendMessageW(h, BM_SETCHECK, c.checked ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(h, c.enabled);
        if (!c.enabled && h == focused) focusLost = true;
        SetWindowPos(h, after, (int)std::lround(m.Px(c.rect.left)), (int)std::lround(m.Px(c.rect.top)),
                     (int)std::lround(m.Px(c.rect.right - c.rect.left)), (int)std::lround(m.Px(c.rect.bottom - c.rect.top)),
                     SWP_NOACTIVATE);
        InvalidateRect(h, nullptr, FALSE);
        after = h;
        keep[c.id] = h;
    }
    for (auto &[id, h] : m.hwnds) {
        if (h == focused) focusLost = true;
        DestroyWindow(h);
    }
    m.hwnds = std::move(keep);
    if (focusLost || !GetFocus()) {
        if (m.defaultId && m.hwnds.count(m.defaultId)) SetFocus(m.hwnds[m.defaultId]);
        else if (!m.controls.empty()) {
            for (auto it = m.controls.rbegin(); it != m.controls.rend(); ++it)
                if (it->enabled) {
                    SetFocus(m.hwnds[it->id]);
                    break;
                }
        } else SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

namespace {

void RoundRect(ID2D1RenderTarget *t, const D2D1_RECT_F &r, float radius, ID2D1Brush *fill, ID2D1Brush *stroke) {
    const D2D1_ROUNDED_RECT rr{r, radius, radius};
    if (fill) t->FillRoundedRectangle(rr, fill);
    if (stroke) {
        const D2D1_ROUNDED_RECT inner{D2D1::RectF(r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f), radius, radius};
        t->DrawRoundedRectangle(inner, stroke, 1);
    }
}

}  // namespace

LRESULT CALLBACK Window::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT Window::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    Impl &m = *impl_;
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        if (m.EnsureTarget() && SUCCEEDED(m.target->BindDC(dc, &rc))) {
            ID2D1DCRenderTarget *t = m.target.Get();
            t->BeginDraw();
            t->Clear(m.palette.bg);
            ComPtr<ID2D1SolidColorBrush> brush;
            t->CreateSolidColorBrush(m.palette.text1, &brush);
            // The footer's band.
            brush->SetColor(m.palette.footer);
            t->FillRectangle(D2D1::RectF(0, kHeight - kFooter, kWidth, kHeight + 100), brush.Get());
            for (const Paint &p : m.paints) {
                switch (p.what) {
                case Paint::What::Text:
                    brush->SetColor(p.color);
                    t->DrawTextLayout(D2D1::Point2F(p.rect.left, p.rect.top), p.layout.Get(), brush.Get());
                    break;
                case Paint::What::Dot:
                    brush->SetColor(p.color);
                    t->FillEllipse(D2D1::Ellipse(D2D1::Point2F((p.rect.left + p.rect.right) / 2, (p.rect.top + p.rect.bottom) / 2), 2, 2),
                                   brush.Get());
                    break;
                case Paint::What::FieldBox: {
                    ComPtr<ID2D1SolidColorBrush> fill, stroke;
                    t->CreateSolidColorBrush(m.palette.field, &fill);
                    t->CreateSolidColorBrush(m.palette.borderStrong, &stroke);
                    RoundRect(t, p.rect, kRadius, fill.Get(), stroke.Get());
                    brush->SetColor(m.palette.text1);
                    t->DrawTextLayout(D2D1::Point2F(p.rect.left + 12, p.rect.top), p.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    break;
                }
                case Paint::What::Track: {
                    brush->SetColor(m.palette.track);
                    t->FillRoundedRectangle(D2D1::RoundedRect(p.rect, 2, 2), brush.Get());
                    if (p.fraction > 0) {
                        brush->SetColor(m.palette.accent);
                        D2D1_RECT_F done = p.rect;
                        done.right = done.left + (float)((p.rect.right - p.rect.left) * p.fraction);
                        t->FillRoundedRectangle(D2D1::RoundedRect(done, 2, 2), brush.Get());
                    }
                    break;
                }
                case Paint::What::Icon: {
                    brush->SetColor(p.color);
                    const wchar_t glyph[2] = {p.glyph, 0};
                    t->DrawTextW(glyph, 1, m.icon.Get(), p.rect, brush.Get());
                    break;
                }
                case Paint::What::Hairline:
                    brush->SetColor(m.palette.border);
                    t->FillRectangle(p.rect, brush.Get());
                    break;
                }
            }
            if (t->EndDraw() == D2DERR_RECREATE_TARGET) m.target.Reset();
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_NOTIFY: {
        auto *nm = reinterpret_cast<NMHDR *>(lp);
        if (nm->code != NM_CUSTOMDRAW) break;
        auto *cd = reinterpret_cast<NMCUSTOMDRAW *>(lp);
        // Buttons ask before erasing too; only the paint stage is drawn here, all of it.
        if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
        const Control *c = nullptr;
        for (const Control &k : m.controls)
            if (k.id == (int)nm->idFrom) c = &k;
        if (!c || !m.EnsureTarget()) return CDRF_DODEFAULT;
        RECT rc;
        GetClientRect(nm->hwndFrom, &rc);
        if (FAILED(m.target->BindDC(cd->hdc, &rc))) return CDRF_DODEFAULT;
        ID2D1DCRenderTarget *t = m.target.Get();
        const float w = (rc.right - rc.left) * 96.f / m.dpi, h = (rc.bottom - rc.top) * 96.f / m.dpi;
        const bool hot = cd->uItemState & CDIS_HOT, pressed = cd->uItemState & CDIS_SELECTED;
        const bool disabled = cd->uItemState & CDIS_DISABLED;
        const bool focus = (cd->uItemState & CDIS_FOCUS) && m.keyboardCues;
        const bool inFooter = c->rect.top >= kHeight - kFooter;
        const Palette &p = m.palette;
        t->BeginDraw();
        t->Clear(inFooter ? p.footer : p.bg);
        ComPtr<ID2D1SolidColorBrush> fill, stroke, text;
        t->CreateSolidColorBrush(p.text1, &fill);
        t->CreateSolidColorBrush(p.borderStrong, &stroke);
        t->CreateSolidColorBrush(p.text1, &text);
        const D2D1_RECT_F all = D2D1::RectF(0, 0, w, h);
        switch (c->kind) {
        case ControlKind::Primary:
        case ControlKind::Secondary:
        case ControlKind::Menu: {
            const bool primary = c->kind == ControlKind::Primary;
            if (primary) {
                fill->SetColor(disabled ? p.hover : pressed ? p.accentPress : hot ? p.accentHover : p.accent);
                text->SetColor(disabled ? p.text3 : p.onAccent);
                RoundRect(t, all, kRadius, fill.Get(), disabled ? stroke.Get() : nullptr);
            } else {
                fill->SetColor(pressed ? p.press : hot ? p.hover : p.surface);
                text->SetColor(disabled ? p.text3 : p.text1);
                RoundRect(t, all, kRadius, fill.Get(), stroke.Get());
            }
            if (c->kind == ControlKind::Menu) {
                ComPtr<IDWriteTextLayout> l = m.Layout(c->label, m.button.Get(), w - 12 - 28, h);
                t->DrawTextLayout(D2D1::Point2F(12, 0), l.Get(), text.Get());
                const wchar_t chevron[2] = {kGlyphChevron, 0};
                text->SetColor(p.text2);
                t->DrawTextW(chevron, 1, m.iconSmall.Get(), D2D1::RectF(w - 28, 0, w - 8, h), text.Get());
            } else {
                ComPtr<IDWriteTextLayout> l = m.Layout(c->label, m.button.Get(), w, h);
                if (l) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                t->DrawTextLayout(D2D1::Point2F(0, 0), l.Get(), text.Get());
            }
            break;
        }
        case ControlKind::Link: {
            text->SetColor(disabled ? p.text3 : pressed ? p.accentPress : p.accent);
            ComPtr<IDWriteTextLayout> l = m.Layout(c->label, m.button.Get(), w, h);
            if (l) {
                l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                if (hot) l->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, (UINT32)c->label.size()});
            }
            t->DrawTextLayout(D2D1::Point2F(0, 0), l.Get(), text.Get());
            break;
        }
        case ControlKind::Check: {
            const bool checked = SendMessageW(nm->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const D2D1_RECT_F box = D2D1::RectF(0, 1, 20, 21);
            if (checked) {
                fill->SetColor(disabled ? p.text3 : pressed ? p.accentPress : hot ? p.accentHover : p.accent);
                RoundRect(t, box, 4, fill.Get(), nullptr);
                const wchar_t mark[2] = {kGlyphCheck, 0};
                text->SetColor(p.onAccent);
                t->DrawTextW(mark, 1, m.iconSmall.Get(), box, text.Get());
            } else {
                fill->SetColor(pressed ? p.press : hot ? p.hover : p.field);
                stroke->SetColor(disabled ? p.borderStrong : p.text3);
                RoundRect(t, box, 4, fill.Get(), stroke.Get());
            }
            text->SetColor(disabled ? p.text3 : p.text1);
            ComPtr<IDWriteTextLayout> l1 = m.Layout(c->label, m.body.Get(), w - 32);
            t->DrawTextLayout(D2D1::Point2F(32, 0), l1.Get(), text.Get());
            if (!c->detail.empty()) {
                text->SetColor(disabled ? p.text3 : p.text2);
                ComPtr<IDWriteTextLayout> l2 = m.Layout(c->detail, m.caption.Get(), w - 32);
                t->DrawTextLayout(D2D1::Point2F(32, Impl::Height(l1.Get()) + 2), l2.Get(), text.Get());
            }
            break;
        }
        }
        if (focus) {
            stroke->SetColor(p.text1);
            const D2D1_RECT_F ring = c->kind == ControlKind::Check ? D2D1::RectF(-1, 0, 22, 22) : D2D1::RectF(1, 1, w - 1, h - 1);
            t->DrawRoundedRectangle(D2D1::RoundedRect(ring, kRadius, kRadius), stroke.Get(), 2);
        }
        if (t->EndDraw() == D2DERR_RECREATE_TARGET) m.target.Reset();
        return CDRF_SKIPDEFAULT;
    }
    case WM_CHANGEUISTATE:
    case WM_UPDATEUISTATE: {
        const LRESULT r = DefWindowProcW(hwnd_, msg, wp, lp);
        m.keyboardCues = !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        for (auto &[id, h] : m.hwnds) InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_SETCURSOR: {
        HWND over = reinterpret_cast<HWND>(wp);
        for (const Control &c : m.controls)
            if (c.kind == ControlKind::Link && m.hwnds.count(c.id) && m.hwnds[c.id] == over && IsWindowEnabled(over)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDCANCEL) {
            OnCloseRequest();
            return 0;
        }
        if (id == IDOK) {
            if (m.defaultId) OnClick(m.defaultId);
            return 0;
        }
        if (HIWORD(wp) == BN_CLICKED) OnClick(id);
        return 0;
    }
    case DM_GETDEFID: return m.defaultId ? MAKELRESULT(m.defaultId, DC_HASDEFID) : 0;
    case WM_CLOSE:
        OnCloseRequest();
        return 0;
    case WM_DESTROY:
        for (auto &[id, h] : m.hwnds) DestroyWindow(h);
        m.hwnds.clear();
        hwnd_ = nullptr;
        PostQuitMessage(code_);
        return 0;
    case WM_DPICHANGED: {
        m.dpi = HIWORD(wp);
        m.target.Reset();
        const RECT *r = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Rebuild();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t *>(lp), L"ImmersiveColorSet") == 0) {
            m.palette = MakePalette(AppsUseDarkTheme());
            StyleCaption(hwnd_, m.palette);
            Rebuild();
            for (auto &[id, h] : m.hwnds) InvalidateRect(h, nullptr, TRUE);
        }
        break;
    default:
        if (msg >= WM_APP && msg < 0xC000) {
            OnPosted(msg, wp, lp);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

/* --- helpers --- */

bool OpenAsDesktopUser(const std::wstring &target, const std::wstring &args) {
    // The desktop's folder view belongs to Explorer, which runs as the signed-in person; its
    // Application object starts programs as Explorer would.
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
    VARIANT empty{};
    long hwnd = 0;
    ComPtr<IDispatch> dispatch;
    if (windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch) != S_OK || !dispatch) return false;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> app;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(dispatch.As(&services)) ||
        FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView)) ||
        FAILED(folderView->get_Application(&app)) || FAILED(app.As(&shell)))
        return false;
    BSTR file = SysAllocString(target.c_str());
    VARIANT vArgs{}, vDir{}, vOp{}, vShow{};
    vArgs.vt = VT_BSTR;
    vArgs.bstrVal = SysAllocString(args.c_str());
    vShow.vt = VT_I4;
    vShow.lVal = SW_SHOWNORMAL;
    const HRESULT hr = shell->ShellExecute(file, vArgs, vDir, vOp, vShow);
    SysFreeString(file);
    VariantClear(&vArgs);
    return SUCCEEDED(hr);
}

std::wstring FormatSize(unsigned long long bytes) {
    const wchar_t *units[] = {L"KB", L"MB", L"GB", L"TB"};
    double v = bytes / 1024.0;
    int u = 0;
    while (v >= 1000 && u < 3) {
        v /= 1024;
        ++u;
    }
    wchar_t buf[32];
    swprintf_s(buf, v < 10 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
    return buf;
}

}  // namespace wizard
