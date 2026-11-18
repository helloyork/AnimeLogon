#include "otherdisplays.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include "animelogon/blur.h"
#include "animelogon/bitmap.h"
#include "animelogon/log.h"
#include "animelogon/monitors.h"
#include "animelogon/resolve.h"
#include "animelogon/rotation.h"
#include "animelogon/text.h"
#include "bake.h"
#include "displaylayout.h"
#include "events.h"
#include "screen.h"

namespace {

// Its own class, so that nothing looking at the Winlogon desktop's windows mistakes these for
// the overlay's own (Presenter::KeepOnTop, for one, puts the overlay back above them).
constexpr wchar_t kClass[] = L"AnimeLogonOtherDisplay";
// How often the password screen is looked at: the displays, the password box, a shutdown.
constexpr ULONGLONG kCheckMs = 500;

// Set by WM_DISPLAYCHANGE reaching one of the windows, for the next look at the displays.
std::atomic<bool> g_displaysChanged{false};

int Width(const RECT &r) { return r.right - r.left; }
int Height(const RECT &r) { return r.bottom - r.top; }

// Each window keeps the display it was made for in its extra bytes: left and top in the first
// pointer-sized slot, right and bottom in the second.
constexpr int kRectSlot = 0;
void KeepRect(HWND hwnd, const RECT &r) {
    SetWindowLongPtrW(hwnd, kRectSlot, (LONG_PTR)(((uint64_t)(uint32_t)r.left << 32) | (uint32_t)r.top));
    SetWindowLongPtrW(hwnd, kRectSlot + sizeof(LONG_PTR), (LONG_PTR)(((uint64_t)(uint32_t)r.right << 32) | (uint32_t)r.bottom));
}
bool KeptRect(HWND hwnd, RECT *r) {
    const uint64_t a = (uint64_t)GetWindowLongPtrW(hwnd, kRectSlot), b = (uint64_t)GetWindowLongPtrW(hwnd, kRectSlot + sizeof(LONG_PTR));
    *r = {(LONG)(int32_t)(a >> 32), (LONG)(int32_t)a, (LONG)(int32_t)(b >> 32), (LONG)(int32_t)b};
    return r->right > r->left && r->bottom > r->top;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: {
        // When its display goes, Windows moves the window onto a display that is still there --
        // usually the primary, over the password box. It is hidden where it stands instead, at
        // once, and closed when the displays are next looked at.
        auto *pos = reinterpret_cast<WINDOWPOS *>(lp);
        RECT own{};
        if (pos && !(pos->flags & SWP_NOMOVE) && KeptRect(hwnd, &own) && (pos->x != own.left || pos->y != own.top)) {
            pos->flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW;
            pos->flags &= ~SWP_SHOWWINDOW;
            g_displaysChanged.store(true);
        }
        break;
    }
    case WM_DISPLAYCHANGE: {
        // Its display is gone: hidden now, before anything can move it, and closed next.
        RECT own{};
        if (KeptRect(hwnd, &own) && !displaylayout::Holds(displaylayout::Current(), own)) ShowWindow(hwnd, SW_HIDE);
        g_displaysChanged.store(true);
        break;
    }
    case WM_DPICHANGED:
        // Sized in physical pixels for the display it covers; a new scale changes nothing here.
        g_displaysChanged.store(true);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterOnce() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.cbWndExtra = 2 * sizeof(LONG_PTR);  // the display it was made for (KeepRect)
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

// Every display, the primary first, then by position: what the stills were made for.
std::vector<RECT> LayoutOf(const plan::Plan &plan) {
    std::vector<displaylayout::Display> displays;
    for (const plan::Display &d : plan.displays) displays.push_back({d.rect, d.primary});
    return displaylayout::Ordered(std::move(displays));
}

std::wstring KindOf(const plan::Wallpaper &w) {
    switch (w.source) {
    case plan::Source::Video: return L"the first frame of video " + w.id;
    case plan::Source::Image: return L"image " + w.id;
    case plan::Source::Scene: return L"the still of scene " + w.id;
    case plan::Source::Gradient: return L"the built-in wallpaper";
    case plan::Source::None: break;
    }
    return L"no wallpaper";
}

// The display's picture of its wallpaper, at its size and its theme's brightness and blur, B G R:
// what the display was showing. A wallpaper that cannot be shown gives way to the built-in one, as it
// does on the screen. `what` says which it is, for the log.
std::vector<uint8_t> Picture(const stills::Still &still, std::wstring *what) {
    std::vector<uint8_t> bgr;
    std::wstring why;
    const bool ok = still.wallpaper.source == plan::Source::Video
                        ? bake::FirstFrameOn(still.wallpaper.path, still.canvas, still.rect, still.wallpaper.fit, &bgr) ||
                              (why = L"its first frame could not be decoded", false)
                        : stills::Compose(still, &bgr, &why);
    if (ok) {
        *what = KindOf(still.wallpaper);
    } else {
        *what = L"the built-in wallpaper, as " + KindOf(still.wallpaper) + L" cannot be shown (" + why + L")";
        bgr = stills::BuiltIn(still);
    }
    animelogon::ApplyBrightness(bgr.data(), bgr.size() / 3, 3, still.wallpaper.brightness);
    // As the screen blurs it: by this display's height, or the primary's when spanning (plan.h).
    const int h = Height(still.rect);
    animelogon::blur::Apply(bgr.data(), Width(still.rect), h, 3, plan::BlurSigma(still.wallpaper, h));
    return bgr;
}

// A top-down 32-bit DIB of the still, ready to be put in a layered window.
HBITMAP MakeBitmap(const stills::Still &still, animelogon::OtherDisplays mode, const std::vector<uint8_t> &bgr) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = Width(still.rect);
    bi.bmiHeader.biHeight = -Height(still.rect);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    stills::Finish(bgr, Width(still.rect), Height(still.rect), mode, static_cast<uint8_t *>(bits));
    return bitmap;
}

// The lowest visible window of this process that is not a still: the overlay's own, while they
// are up. The stills go directly under it.
HWND LowestOfOurs() {
    const DWORD self = GetCurrentProcessId();
    HWND lowest = nullptr;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != self || !IsWindowVisible(w)) continue;
        wchar_t cls[64] = L"";
        GetClassNameW(w, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, kClass) != 0) lowest = w;
    }
    return lowest;
}

std::wstring ImageOf(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"?";
    wchar_t image[MAX_PATH] = L"";
    DWORD n = ARRAYSIZE(image);
    const bool ok = QueryFullProcessImageNameW(p, 0, image, &n) != FALSE;
    CloseHandle(p);
    const wchar_t *name = ok ? wcsrchr(image, L'\\') : nullptr;
    return name ? name + 1 : L"?";
}

int AlphaOf(float k) { return (int)std::lround(255.0f * std::clamp(k, 0.0f, 1.0f)); }

}  // namespace

// The stills being made, off the frame thread. The thread fills `made` in and then sets `done`;
// until then only it touches them.
struct OtherDisplays::Job {
    struct Made {
        stills::Still still;
        HBITMAP bitmap = nullptr;  // null if it could not be made
        std::wstring what;         // what it shows, or why there is none
        ULONGLONG ms = 0;          // how long it took
    };
    std::vector<Made> made;
    animelogon::OtherDisplays mode = animelogon::OtherDisplays::Blur;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};

    ~Job() {
        for (Made &m : made)
            if (m.bitmap) DeleteObject(m.bitmap);
    }

    static void Run(const std::shared_ptr<Job> &job) {
        // Below the frame thread and the video decoders: this can wait, they cannot.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (Made &m : job->made) {
            if (job->cancelled.load()) break;
            const ULONGLONG start = GetTickCount64();
            const std::vector<uint8_t> bgr = Picture(m.still, &m.what);
            m.bitmap = MakeBitmap(m.still, job->mode, bgr);
            if (!m.bitmap) m.what = animelogon::Format(L"no %dx%d bitmap could be made", Width(m.still.rect), Height(m.still.rect));
            m.ms = GetTickCount64() - start;
        }
        if (SUCCEEDED(co)) CoUninitialize();
        job->done.store(true);
    }
};

// At the process's end the windows go with it; a still still being made is only told to stop.
OtherDisplays::~OtherDisplays() {
    if (job_) job_->cancelled.store(true);
}

void OtherDisplays::Prepare(const plan::Plan &plan, animelogon::OtherDisplays mode, float wakeSeconds) {
    Clear();
    wakeSeconds_ = wakeSeconds;
    Start(plan, mode);
}

void OtherDisplays::Start(const plan::Plan &plan, animelogon::OtherDisplays mode) {
    mode_ = mode;
    layout_ = LayoutOf(plan);
    g_displaysChanged.store(false);
    const std::vector<stills::Still> chosen = stills::Choose(plan, mode);
    // Said only where there is more than one display, so a single display's log is as it was.
    if (plan.displays.size() > 1)
        ALOG(L"other displays: other_displays = %s -- %zu of the %zu other display(s) get a still", animelogon::ToString(mode),
             chosen.size(), plan.displays.size() - 1);
    if (chosen.empty()) return;
    auto job = std::make_shared<Job>();
    job->mode = mode;
    for (const stills::Still &s : chosen) job->made.push_back({s});
    try {
        std::thread([job] { Job::Run(job); }).detach();
    } catch (const std::exception &) {
        ALOG(L"other displays: no thread could make the stills -- they stay black");
        return;
    }
    job_ = std::move(job);
    adopted_ = 0;
}

void OtherDisplays::Clear() {
    if (job_) job_->cancelled.store(true);
    job_.reset();
    adopted_ = 0;
    for (Window &w : windows_)
        if (w.hwnd) DestroyWindow(w.hwnd);
    if (!windows_.empty()) ALOG(L"other displays: %zu still(s) taken down", windows_.size());
    windows_.clear();
    woken_ = false;
    reported_ = false;
    stopped_ = false;
    layout_.clear();
}

// Puts the next finished still into a hidden window, its picture already handed to the
// compositor, so that showing it later is one call. False when there is none to take.
bool OtherDisplays::AdoptOne() {
    if (!job_ || !job_->done.load()) return false;
    while (adopted_ < job_->made.size()) {
        Job::Made &m = job_->made[adopted_++];
        const stills::Still &s = m.still;
        if (!m.bitmap) {
            ALOG(L"other displays: display %s is left black -- %s", s.monitorKey.c_str(), m.what.c_str());
            continue;
        }
        HWND hwnd = RegisterOnce() ? CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                                          WS_EX_TOPMOST,
                                                      kClass, L"", WS_POPUP, s.rect.left, s.rect.top, Width(s.rect),
                                                      Height(s.rect), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
                                     : nullptr;
        bool ok = hwnd != nullptr;
        if (ok) {
            KeepRect(hwnd, s.rect);
            HDC screenDc = GetDC(nullptr);
            HDC memory = CreateCompatibleDC(screenDc);
            const HGDIOBJ old = SelectObject(memory, m.bitmap);
            POINT at{s.rect.left, s.rect.top}, origin{0, 0};
            SIZE size{Width(s.rect), Height(s.rect)};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};
            ok = UpdateLayeredWindow(hwnd, screenDc, &at, &size, memory, &origin, 0, &blend, ULW_ALPHA) != FALSE;
            SelectObject(memory, old);
            DeleteDC(memory);
            ReleaseDC(nullptr, screenDc);
        }
        DeleteObject(m.bitmap);
        m.bitmap = nullptr;
        if (!ok) {
            ALOG(L"other displays: display %s is left black -- its window could not be made (%lu)", s.monitorKey.c_str(),
                 GetLastError());
            if (hwnd) DestroyWindow(hwnd);
            continue;
        }
        Window w;
        w.still = s;
        w.hwnd = hwnd;
        w.alpha = 0;
        windows_.push_back(w);
        ALOG(L"other displays: display %s %dx%d gets %s, %s, made in %llu ms", s.monitorKey.c_str(), Width(s.rect),
             Height(s.rect), m.what.c_str(), mode_ == animelogon::OtherDisplays::Blur ? L"blurred" : L"as it is", m.ms);
        return true;
    }
    job_.reset();
    return false;
}

void OtherDisplays::SetAlpha(Window &w, int alpha) {
    if (alpha == w.alpha) return;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(w.hwnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
    w.alpha = alpha;
}

void OtherDisplays::Show(Window &w, int alpha) {
    SetAlpha(w, alpha);
    // Directly under our own windows while they are up, so they still cover it until the wake
    // transition uncovers it; at the top otherwise.
    const HWND under = LowestOfOurs();
    SetWindowPos(w.hwnd, under ? under : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    w.shown = true;
}

void OtherDisplays::CloseVanished() {
    const std::vector<RECT> now = displaylayout::Current();
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (displaylayout::Holds(now, it->still.rect)) {
            ++it;
            continue;
        }
        ALOG(L"other displays: display %s is gone -- its still is closed", it->still.monitorKey.c_str());
        DestroyWindow(it->hwnd);
        it = windows_.erase(it);
    }
}

void OtherDisplays::Live(bool woken, float progress) {
    // A still whose display went is closed before Windows can move it onto another one.
    if (!windows_.empty() && g_displaysChanged.load()) CloseVanished();
    AdoptOne();
    if (!woken) return;
    if (!woken_) {
        // The wake: what is ready goes in whole, under the windows that are about to uncover it.
        woken_ = true;
        for (Window &w : windows_) Show(w, 255);
        if (!windows_.empty() || job_)
            ALOG(L"other displays: %zu still(s) under the screen for the wake, %zu still being made", windows_.size(),
                 job_ ? job_->made.size() - adopted_ : (size_t)0);
        return;
    }
    // Made since: in step with the transition, so it is whole when the transition is over.
    for (Window &w : windows_) {
        if (!w.shown) {
            w.follows = true;
            Show(w, AlphaOf(progress));
            ALOG(L"other displays: display %s's still came late -- fading in with the rest of the transition",
                 w.still.monitorKey.c_str());
        } else if (w.follows) {
            SetAlpha(w, AlphaOf(progress));
        }
    }
}

bool OtherDisplays::Parked() {
    if (!woken_) {
        Clear();
        return false;
    }
    // Nothing else is being drawn now: every finished still is taken at once.
    while (AdoptOne()) {
    }
    const ULONGLONG now = GetTickCount64();
    // A cut has no length: what comes late comes at once.
    const float span = std::max(wakeSeconds_, 0.001f) * 1000.0f;
    bool fading = false;
    for (Window &w : windows_) {
        if (!w.shown) {
            w.appearAt = now;
            Show(w, 0);
            ALOG(L"other displays: display %s's still came after the transition -- fading in", w.still.monitorKey.c_str());
        } else if (w.follows) {
            // The transition ended before it was whole: it carries on from where it got to.
            w.follows = false;
            w.appearAt = now - (ULONGLONG)(span * (float)w.alpha / 255.0f);
        }
        if (!w.appearAt) continue;
        const float k = (float)(now - w.appearAt) / span;
        SetAlpha(w, AlphaOf(k));
        if (k < 1.0f) fading = true;
        else w.appearAt = 0;
    }
    // A display that went is dealt with at once, not at the next regular look.
    if (now - checkedAt_ >= kCheckMs || g_displaysChanged.load()) {
        checkedAt_ = now;
        Check();
    }
    if (!reported_ && !fading && !windows_.empty() && !job_) {
        reported_ = true;
        Report();
    }
    return fading;
}

void OtherDisplays::Check() {
    // Windows' shutdown screen is never under anything of ours.
    if (screen::ShutdownUnderway() || events::IsSet(events::kGoingDown)) {
        if (!stopped_) ALOG(L"other displays: Windows is shutting down -- the stills are taken down");
        stopped_ = true;
        if (job_) job_->cancelled.store(true);
        job_.reset();
        for (Window &w : windows_) DestroyWindow(w.hwnd);
        windows_.clear();
        return;
    }
    if (stopped_) return;

    // Made for displays that may not be there any more: made again, as the displays are now.
    // Some changes post no WM_DISPLAYCHANGE, so the layout itself is compared too.
    if (g_displaysChanged.exchange(false) || !displaylayout::Same(displaylayout::Current(), layout_)) {
        CloseVanished();
        ALOG(L"other displays: the displays changed -- making the stills again");
        if (job_) job_->cancelled.store(true);
        job_.reset();
        for (Window &w : windows_) DestroyWindow(w.hwnd);
        windows_.clear();
        std::wstring why;
        const animelogon::Settings settings = animelogon::rotation::LoadShown(&why);
        Start(plan::Build(settings, animelogon::EnumerateMonitors(), animelogon::DiskStore()), settings.otherDisplays);
        reported_ = false;
        return;
    }

    // The password box is never under a still, wherever Windows puts it.
    RECT box{};
    const wchar_t *why = L"";
    if (screen::CredentialMonitor(&box, &why)) {
        for (auto it = windows_.begin(); it != windows_.end();) {
            if (EqualRect(&it->still.rect, &box)) {
                ALOG(L"other displays: the password box is on display %s (%s) -- its still is taken down",
                     it->still.monitorKey.c_str(), why);
                DestroyWindow(it->hwnd);
                it = windows_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// Once on each password screen, for the log: whether anything of another program's lies over a
// still, which would hide it.
void OtherDisplays::Report() {
    const DWORD self = GetCurrentProcessId();
    for (const Window &w : windows_) {
        std::wstring over;
        for (HWND h = GetTopWindow(nullptr); h && h != w.hwnd; h = GetWindow(h, GW_HWNDNEXT)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            RECT r{}, both{};
            if (pid == self || !IsWindowVisible(h) || !GetWindowRect(h, &r) || !IntersectRect(&both, &r, &w.still.rect)) continue;
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            over = animelogon::Format(L" -- under %s's %s window", ImageOf(pid).c_str(), cls);
            break;
        }
        ALOG(L"other displays: display %s shows its still%s", w.still.monitorKey.c_str(), over.c_str());
    }
}

bool OtherDisplays::Preview(const std::wstring &dir) {
    std::wstring why;
    const animelogon::Settings settings = animelogon::rotation::LoadShown(&why);
    const plan::Plan plan = plan::Build(settings, animelogon::EnumerateMonitors(), animelogon::DiskStore());
    const std::vector<stills::Still> chosen = stills::Choose(plan, settings.otherDisplays);
    std::wstring report = animelogon::Format(L"other_displays = %s\r\n", animelogon::ToString(settings.otherDisplays));
    bool ok = !plan.displays.empty();
    for (size_t i = 0; i < plan.displays.size(); ++i) {
        const plan::Display &d = plan.displays[i];
        const stills::Still still{d.monitorKey, d.rect, d.canvas, d.wallpaper};
        const bool gets = std::any_of(chosen.begin(), chosen.end(), [&](const stills::Still &c) { return c.monitorKey == d.monitorKey; });
        std::wstring what = L"black";
        if (d.wallpaper.source != plan::Source::None && Width(d.rect) > 0 && Height(d.rect) > 0) {
            const std::vector<uint8_t> bgr = Picture(still, &what);
            for (const animelogon::OtherDisplays mode : {animelogon::OtherDisplays::Picture, animelogon::OtherDisplays::Blur}) {
                std::vector<uint8_t> bgra((size_t)Width(d.rect) * Height(d.rect) * 4);
                stills::Finish(bgr, Width(d.rect), Height(d.rect), mode, bgra.data());
                const std::vector<uint8_t> file = animelogon::bitmap::Build(Width(d.rect), Height(d.rect), bgra.data(), bgra.size());
                const std::wstring path = animelogon::Format(L"%s\\display%zu-%s.bmp", dir.c_str(), i + 1, animelogon::ToString(mode));
                HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD wrote = 0;
                ok = ok && h != INVALID_HANDLE_VALUE && !file.empty() &&
                     WriteFile(h, file.data(), (DWORD)file.size(), &wrote, nullptr) && wrote == file.size();
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        }
        report += animelogon::Format(L"display%zu %s%s %ld,%ld %dx%d canvas %ld,%ld %dx%d: %s; %s\r\n", i + 1,
                                     d.monitorKey.c_str(), d.primary ? L" (primary)" : L"", d.rect.left, d.rect.top,
                                     Width(d.rect), Height(d.rect), d.canvas.left, d.canvas.top, Width(d.canvas),
                                     Height(d.canvas), what.c_str(), gets ? L"gets a still" : L"gets none");
    }
    const std::string text = animelogon::ToUtf8(report);
    HANDLE h = CreateFileW((dir + L"\\stills.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    ok = ok && h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}
