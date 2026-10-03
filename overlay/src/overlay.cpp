// overlay.exe: shows each display's theme -- its wallpaper, a video or a picture, and the
// components over it -- on the Winlogon desktop until a key or click, then fades out and hands
// the screen back to Windows. Started as SYSTEM by the service, one per logon screen. It parks
// between appearances: once dismissed it stays out of the way until the session is used and
// locked again, so it never fights somebody signing in. `--windowed` runs it on the ordinary
// desktop, showing the primary display's theme in a window, for looking at a change without a
// lock screen.

#include <windows.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shellscalingapi.h>

#include <wtsapi32.h>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/resolve.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"

#include "audio.h"
#include "bake.h"
#include "clocktext.h"
#include "componentlayer.h"
#include "events.h"
#include "inputsink.h"
#include "keyhook.h"
#include "plan.h"
#include "player.h"
#include "presenter.h"
#include "screen.h"
#include "wake.h"

namespace {

constexpr wchar_t kWindowClass[] = L"AnimeLogonOverlay";
constexpr int kRescueHotkeyId = 0xA10E;  // Ctrl+Alt+F11
constexpr float kFadeSeconds = 0.5f;
// The components fade in once the wallpaper is up.
constexpr float kClockFadeInSeconds = 0.4f;
// How long after the compositor's frame the next picture is drawn: right after one frame
// leaves nearly a whole frame of slack for the next, which a late wake-up would not.
constexpr double kAfterComposeSeconds = 0.001;
// A live screen whose loop has not turned for this long is ended, so it can never hold the
// password box hostage.
constexpr ULONGLONG kHangMs = 5000;
// Exit code telling the launcher to start the next overlay parked (see kDismissedFlag).
constexpr int kExitDismissed = 3;
constexpr wchar_t kDismissedFlag[] = L"--dismissed";
// How often a parked overlay looks at whether the sign-in background needs baking again.
constexpr ULONGLONG kBakeCheckMs = 5000;
// How long the windows stay hidden waiting for the first frame. Until they show, Windows'
// own background -- that same frame -- is what is on the screen.
constexpr ULONGLONG kFirstFrameWaitMs = 1500;

struct Options {
    bool windowed = false;   // run on the ordinary desktop, for development
    bool now = false;        // skip the parked wait for a logon screen
    bool dismissed = false;  // the previous overlay was waved away: wait for the screen to be used
    bool still = false;      // with --windowed: input does not wake it, for screenshots
    int seconds = 0;         // exit after this many seconds, for testing; 0 is forever
    std::wstring bakePreview;  // write the sign-in background this would bake there, and exit
};

// Why a live appearance ended, and so what to do next.
enum class LiveEnd {
    Leave,      // the service is stopping, shutting down, paused, or the device was lost: exit
    Dismissed,  // somebody arrived (or there was nothing to show): park until the screen is used
    LogonGone,  // the logon screen went away on its own: park for the next lock
    Rebuild,    // the displays changed under it: go live again on the new layout
};

struct Overlay {
    HWND inputWindow = nullptr;  // the primary window, which owns raw input and the hotkey
    wake::Intent intent;
    bool woke = false;
    bool keyhookOn = false;
    ULONGLONG fadeStart = 0;
    bool displayOn = true;
    ULONGLONG displayOffAt = 0;
    DWORD displayOffInput = 0;
    bool leaving = false;        // WM_ENDSESSION / shutdown / rescue asked us to go
    bool displaysChanged = false; // a resolution, scale or monitor change while live
    DWORD session = 0xFFFFFFFF;
    bool lockNotified = false;   // WTS_SESSION_LOCK seen, and no unlock or logon since
    bool hadUser = false;        // somebody has been signed in to this session
    ULONGLONG seenAt = 0;        // when the parked wait saw the logon screen
};

Overlay g_overlay;
const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

// The watchdog's view of the main thread.
std::atomic<bool> g_live{false};
std::atomic<ULONGLONG> g_heartbeat{0};

void Beat() { g_heartbeat.store(GetTickCount64()); }

bool g_still = false;

// `how` says what woke it, for the log; which key is never recorded.
void Wake(const wchar_t *how) {
    if (g_overlay.woke || g_still) return;
    g_overlay.woke = true;
    g_overlay.fadeStart = GetTickCount64();
    // A click wakes too; after any wake no further key is withheld.
    keyhook::State().armed.store(false);
    ALOG(L"wake: %s -- fading out", how);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_overlay.lockNotified = true;
        else if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOGON || wp == WTS_SESSION_LOGOFF)
            g_overlay.lockNotified = false;
        return 0;
    case WM_INPUT:
        if (hwnd == g_overlay.inputWindow) {
            if (inputsink::ReadMessage(reinterpret_cast<HRAWINPUT>(lp), g_overlay.intent, (DWORD)GetMessageTime()))
                Wake(L"a press seen by raw input");
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case keyhook::kWakeMessage:  // a withheld wake key, from the hook thread
        Wake(L"the wake key, withheld");
        return 0;
    case WM_HOTKEY:
        if (wp == kRescueHotkeyId) {
            ALOG(L"rescue: Ctrl+Alt+F11 -- taking the video off and pausing it");
            g_overlay.leaving = true;
            animelogon::machine::WritePausedMarker(L"Ctrl+Alt+F11 on the logon screen");
            PostQuitMessage(0);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            const auto *s = reinterpret_cast<const POWERBROADCAST_SETTING *>(lp);
            if (IsEqualGUID(s->PowerSetting, kConsoleDisplayState) && s->DataLength >= sizeof(DWORD)) {
                const DWORD state = *reinterpret_cast<const DWORD *>(s->Data);
                const bool on = state != 0;  // 0 off, 1 on, 2 dimmed (on)
                if (on != g_overlay.displayOn) {
                    g_overlay.displayOn = on;
                    keyhook::State().displayOff.store(!on);
                    if (!on) {
                        LASTINPUTINFO li{sizeof(li), 0};
                        g_overlay.displayOffInput = GetLastInputInfo(&li) ? li.dwTime : 0;
                        g_overlay.displayOffAt = GetTickCount64();
                        keyhook::DarkenLatch();
                    } else {
                        ALOG(L"power: the display is on again after %llu ms",
                             g_overlay.displayOffAt ? GetTickCount64() - g_overlay.displayOffAt : 0ull);
                    }
                }
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_overlay.displaysChanged = true;
        return msg == WM_DPICHANGED ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ENDSESSION:
        if (wp && !(lp & ENDSESSION_LOGOFF)) {
            ALOG(L"shutdown: WM_ENDSESSION -- off the screen");
            g_overlay.leaving = true;
            PostQuitMessage(0);
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

Options ParseOptions(int argc, wchar_t **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--windowed") o.windowed = true;
        else if (a == L"--now") o.now = true;
        else if (a == kDismissedFlag) o.dismissed = true;
        else if (a == L"--still") o.still = true;
        else if (a == L"--bake-preview" && i + 1 < argc) o.bakePreview = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) o.seconds = _wtoi(argv[++i]);
    }
    return o;
}

void DeclareDpi() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        if (auto set = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
            if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

// Pumps the thread's messages. Returns false if a WM_QUIT was seen.
bool PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// The baked sign-in background, ready on the device. It is written by this process into the
// administrators-only data directory, and checked like everything else read as SYSTEM.
bool LoadStill(Presenter &presenter) {
    const std::wstring path = animelogon::paths::BackgroundPath();
    std::wstring why;
    if (!animelogon::secure::IsTrusted(path, &why)) return false;
    return presenter.LoadStill(path);
}

// The parked wait: return when a logon screen is the desktop on the glass, or the service is
// stopping. `dismissed` is true after somebody has waved the video away; while it is, the wait
// holds -- without re-covering the screen -- until the session is actually used (signed in and
// unlocked, or the secure desktop is gone), so the video never fights somebody signing in. It is
// cleared there, and the next genuine lock brings the video back. Returns false to exit.
bool WaitForLogonScreen(Presenter &presenter, bool *dismissed) {
    ULONGLONG bakedAt = 0;
    for (;;) {
        Beat();
        if (events::IsSet(events::kStop) || animelogon::machine::Paused()) return false;
        if (!PumpMessages()) return false;
        const bool secure = screen::InputDesktopIsSecure();
        const screen::Session s = screen::ReadSession(g_overlay.session);
        if (s.signedIn) g_overlay.hadUser = true;

        // The background is only ever rewritten while somebody is using the session.
        if (!secure && s.console && s.signedIn && !s.locked && !g_overlay.lockNotified &&
            GetTickCount64() - bakedAt > kBakeCheckMs) {
            bake::Refresh();
            LoadStill(presenter);
            bakedAt = GetTickCount64();
        }

        // A dismissed appearance stays dismissed until the screen is used again.
        if (*dismissed) {
            if (!secure || (s.signedIn && !s.locked && !g_overlay.lockNotified)) {
                *dismissed = false;
            } else {
                Sleep(200);
                continue;
            }
        }

        screen::GateInput in;
        in.secure = secure;
        in.session = s;
        in.hadUser = g_overlay.hadUser;
        in.shutdown = screen::ShutdownUnderway();
        in.lockNotified = g_overlay.lockNotified;
        if (screen::Decide(in) == screen::Gate::GoLive) {
            g_overlay.seenAt = GetTickCount64();
            return true;
        }
        Sleep(32);
    }
}

double RefreshSeconds() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return 1.0 / dm.dmDisplayFrequency;
    return 1.0 / 60.0;
}

// Development: the primary display's theme, in a window on the ordinary desktop.
plan::Plan WindowedPlan(plan::Plan plan) {
    if (plan.displays.empty()) return plan;
    plan::Display d = plan.displays.front();
    d.rect = {d.rect.left + 80, d.rect.top + 80, d.rect.left + 80 + 960, d.rect.top + 80 + 540};
    d.canvas = d.rect;
    d.showComponents = true;
    plan.displays = {d};
    return plan;
}

// One appearance: resolve each display's theme, cover the screen, and show the wallpapers and
// their components until somebody arrives or the screen is handed back. Returns why it ended.
LiveEnd GoLiveOnce(Presenter &presenter, const Options &opt, HINSTANCE instance) {
    using Kind = Presenter::Picture::Kind;

    // Fresh state for this appearance.
    g_overlay.inputWindow = nullptr;
    g_overlay.woke = false;
    g_overlay.keyhookOn = false;
    g_overlay.fadeStart = 0;
    g_overlay.displayOn = true;
    g_overlay.displayOffAt = 0;
    g_overlay.displayOffInput = 0;
    g_overlay.leaving = false;
    g_overlay.displaysChanged = false;

    // A device lost while parked is replaced by starting a fresh process.
    if (presenter.DeviceLost()) {
        ALOG(L"overlay: the graphics device was lost while parked -- exiting so a fresh one starts");
        return LiveEnd::Leave;
    }

    // settings.ini holds only ids and enums, never a path, so it is loaded without a trust
    // check. Resolving the themes checks every file they lead to -- theme.xml, wallpaper.ini,
    // the video, the image, component.xml -- before the SYSTEM decoder or reader touches it.
    std::wstring why;
    animelogon::Settings settings = animelogon::LoadSettings(false, &why);
    if (!why.empty()) ALOG(L"settings: %s -- using defaults", why.c_str());

    plan::Plan plan = plan::Build(settings, animelogon::EnumerateMonitors(), animelogon::DiskStore());
    if (opt.windowed) plan = WindowedPlan(std::move(plan));
    for (const std::wstring &p : plan.problems) ALOG(L"theme: %s", p.c_str());
    for (const plan::Display &d : plan.displays) ALOG(L"theme: %s", plan::Describe(d).c_str());

    // One window per display. `shown[i]` is what targets[i] came from.
    std::vector<Presenter::Target> targets;
    std::vector<const plan::Display *> shown;
    for (const plan::Display &d : plan.displays) {
        Presenter::Target t;
        t.rect = d.rect;
        t.canvas = d.canvas;
        t.wallpaper = d.wallpaper;
        t.showComponents = d.showComponents;
        targets.push_back(t);
        shown.push_back(&d);
    }

    // A wallpaper that cannot be shown after all -- a video that does not open or stops
    // decoding, an image that cannot be read -- gives way to the built-in one on every display
    // showing it, as ResolveTheme does for one that does not load, and the screen stays covered.
    // `id` is taken by value: it may be a target's own, which this replaces.
    auto fallBack = [&](std::wstring id, const std::wstring &reason) {
        std::vector<plan::Wallpaper> now(targets.size());
        for (size_t i = 0; i < targets.size(); ++i) now[i] = targets[i].wallpaper;
        const std::vector<size_t> changed = plan::ShowBuiltInInstead(&now, id);
        if (changed.empty()) return;
        ALOG(L"overlay: wallpaper %s cannot be shown (%s) -- showing the built-in wallpaper on %zu display(s)",
             id.c_str(), reason.c_str(), changed.size());
        for (size_t i : changed) {
            targets[i].wallpaper = now[i];
            presenter.SetWallpaper(i, now[i]);
        }
    };

    // Videos by wallpaper id, each opened once however many displays show it.
    std::map<std::wstring, std::shared_ptr<VideoPlayer>> players;
    std::set<std::wstring> unplayable;
    auto openPlayers = [&] {
        for (const Presenter::Target &t : targets) {
            const plan::Wallpaper &w = t.wallpaper;
            if (w.source != plan::Source::Video || players.count(w.id) || unplayable.count(w.id)) continue;
            auto player = std::make_shared<VideoPlayer>();
            if (player->Open(w.path, presenter.device(), presenter.videoManager())) {
                players[w.id] = player;
            } else {
                ALOG(L"overlay: %s could not be opened", w.id.c_str());
                unplayable.insert(w.id);
            }
        }
    };
    auto replaceUnplayable = [&] {
        for (const std::wstring &id : unplayable) fallBack(id, L"it could not be opened");
    };

    // Image wallpapers start reading at once, off this thread. One the device still holds from
    // an earlier appearance, its file unchanged, is ready now.
    {
        std::set<std::wstring> images;
        for (const Presenter::Target &t : targets)
            if (t.wallpaper.source == plan::Source::Image && images.insert(t.wallpaper.id).second)
                presenter.RequestImage(t.wallpaper.id, t.wallpaper.path);
        presenter.KeepImages(images);
    }

    // The screen is covered first, so that Windows' credential screen is never seen. A
    // wallpaper that is ready at once -- an image already read, the built-in one, black -- is
    // the cover itself. Otherwise the baked still is: the picture Windows is already showing,
    // which the wallpaper replaces when it is ready. Without a still, nothing is shown until
    // there is a wallpaper to show.
    bool allReady = !targets.empty();
    for (const Presenter::Target &t : targets) {
        const plan::Source s = t.wallpaper.source;
        allReady = allReady && s != plan::Source::Video &&
                   (s != plan::Source::Image ||
                    presenter.ImageStatus(t.wallpaper.id) == Presenter::ImageState::Ready);
    }
    const bool still = !opt.windowed && !targets.empty() && !allReady && LoadStill(presenter);
    const bool cover = !opt.windowed && !targets.empty() && (allReady || still);
    if (!cover) {
        openPlayers();
        // A display whose video cannot be opened gets the built-in wallpaper.
        if (!opt.windowed) replaceUnplayable();
    }
    if (presenter.DeviceLost()) {
        ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
        return LiveEnd::Leave;
    }
    if (targets.empty()) {
        ALOG(L"overlay: no displays to cover -- leaving the screen to Windows");
        return LiveEnd::Dismissed;  // park until the screen is used
    }

    if (!presenter.CreateWindows(targets, kWindowClass, instance)) {
        ALOG(L"overlay: could not create the windows");
        return LiveEnd::Leave;
    }
    g_overlay.inputWindow = presenter.primary();

    // What each window shows now: its wallpaper when that is ready, else the still if the
    // screen was covered with it, else black. `ready` turns false while a video has no frame yet
    // or an image is still being read; `changed` turns true when any window's picture is new.
    std::vector<VideoPlayer::Frame> held(targets.size());
    std::vector<Kind> drawn(targets.size(), Kind::Black);
    auto compose = [&](double t, bool *ready, bool *changed, bool *firstFresh) {
        std::vector<Presenter::Picture> pictures(targets.size());
        for (size_t i = 0; i < targets.size(); ++i) {
            Presenter::Picture &p = pictures[i];
            p.kind = still ? Kind::Still : Kind::Black;
            const plan::Wallpaper &w = targets[i].wallpaper;
            switch (w.source) {
            case plan::Source::Video: {
                const auto it = players.find(w.id);
                bool fresh = false;
                if (it != players.end() && it->second->FrameAt(t, &held[i], &fresh)) {
                    p.kind = Kind::Video;
                    p.frame = &held[i];
                    p.videoW = it->second->width();
                    p.videoH = it->second->height();
                    if (fresh) *changed = true;
                    if (fresh && i == 0 && firstFresh) *firstFresh = true;
                } else if (it != players.end()) {
                    *ready = false;
                }
                break;
            }
            case plan::Source::Image: {
                std::wstring failure;
                const Presenter::ImageState state = presenter.ImageStatus(w.id, &failure);
                if (state == Presenter::ImageState::Reading) {
                    *ready = false;
                } else {
                    if (state == Presenter::ImageState::Failed) fallBack(w.id, failure);
                    p.kind = Kind::Wallpaper;
                }
                break;
            }
            case plan::Source::Gradient:
                p.kind = Kind::Wallpaper;
                break;
            case plan::Source::None:
                p.kind = Kind::Black;
                break;
            }
            if (p.kind != drawn[i]) *changed = true;
        }
        return pictures;
    };
    auto rendered = [&](const std::vector<Presenter::Picture> &pictures) {
        for (size_t i = 0; i < pictures.size(); ++i) drawn[i] = pictures[i].kind;
    };

    if (cover) {
        bool ready = true, changed = false;
        const std::vector<Presenter::Picture> pictures = compose(0.0, &ready, &changed, nullptr);
        presenter.Render(pictures, 0.0f, 1.0f);
        rendered(pictures);
        presenter.Show();
        ALOG(L"cover: %s is up, %llu ms after the logon screen", still ? L"the sign-in background" : L"the wallpaper",
             GetTickCount64() - g_overlay.seenAt);
    }

    // The components are drawn into the wallpaper's own frames, so they fade out with it. They
    // are set up after the cover and fade in once the wallpaper is up, so the cover never waits
    // for them. Instances drawn alike on several displays share one view, with a cache per window.
    ComponentLayer layer;
    bool withComponents = false;
    {
        bool any = false;
        for (const plan::Display *d : shown) any = any || !d->components.empty();
        if (any && layer.Init(presenter.device())) {
            const animelogon::RegionalFormat format = clocktext::UserFormat();
            constexpr size_t kNone = (size_t)-1;
            std::map<std::wstring, size_t> byKey;
            std::vector<std::vector<size_t>> perTarget(targets.size());
            for (size_t i = 0; i < shown.size(); ++i) {
                for (const plan::Component &c : shown[i]->components) {
                    auto it = byKey.find(c.key);
                    if (it == byKey.end()) {
                        size_t index = kNone;
                        if (!layer.Add(c.drawing, settings.clock, format, &index)) {
                            ALOG(L"components: %s could not be set up -- left out", c.key.c_str());
                            index = kNone;
                        }
                        it = byKey.emplace(c.key, index).first;
                    }
                    if (it->second != kNone) perTarget[i].push_back(it->second);
                }
            }
            withComponents = layer.size() > 0;
            if (withComponents) {
                presenter.SetComponents(&layer, perTarget);
                presenter.SetComponentOpacity(0.0f);
                ALOG(L"components: %zu instance(s) set up", layer.size());
            }
        }
    }
    // By default the components go where Windows puts its password box, which LogonUI may not
    // have drawn yet; it is looked for again below until found.
    bool componentsPlaced =
        !withComponents || opt.windowed || plan.componentDisplays != animelogon::ComponentDisplays::Auto;
    auto placeComponents = [&] {
        RECT monitor{};
        if (!componentsPlaced && screen::CredentialMonitor(&monitor) && presenter.MoveComponentsTo(monitor))
            componentsPlaced = true;
    };
    placeComponents();

    // Without raw input nothing could wake the screen, so it is never covered.
    if (!inputsink::Register(g_overlay.inputWindow)) {
        ALOG(L"overlay: raw input could not be registered (%lu) -- leaving the screen to Windows", GetLastError());
        presenter.DestroyWindows();
        g_overlay.inputWindow = nullptr;
        return LiveEnd::Dismissed;
    }
    g_overlay.intent.Prime(wake::CaptureKeys(), GetTickCount());
    if (!RegisterHotKey(g_overlay.inputWindow, kRescueHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11))
        ALOG(L"overlay: the rescue hotkey could not be registered (%lu)", GetLastError());
    if (!opt.windowed) {
        keyhook::ResetLatch();
        keyhook::State().notify.store(g_overlay.inputWindow);
        keyhook::State().displayOff.store(false);
        keyhook::State().armed.store(true);
        g_overlay.keyhookOn = keyhook::Install();
    }
    HPOWERNOTIFY power =
        RegisterPowerSettingNotification(g_overlay.inputWindow, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (cover) {
        openPlayers();
        replaceUnplayable();
    }
    bool done = false;
    LiveEnd end = LiveEnd::Leave;
    if (presenter.DeviceLost()) {
        ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
        done = true;
    }

    // The audio configuration is global (the audio system's own); the only source a theme has
    // is its wallpaper's sound track, when the wallpaper is a video with one.
    AudioSystem audio;
    audio.Open(settings.audio);
    std::wstring track;  // the wallpaper whose sound track plays, if any
    const double refresh = RefreshSeconds();
    if (!opt.windowed && !done) {
        AudioSystem::RememberConsoleDefault();
        const plan::Wallpaper &first = targets.front().wallpaper;
        if (first.source == plan::Source::Video && !first.audioPath.empty()) {
            audio.PlayVideoTrack(first.audioPath, 0.0, kFadeSeconds);
            track = first.id;
        }
    }

    // A video that stops decoding gives way to the built-in wallpaper, and its sound track
    // stops with it. Returns false when the cause was a lost device, which ends the appearance.
    auto replaceFailed = [&] {
        for (auto it = players.begin(); it != players.end();) {
            if (!it->second->failed()) {
                ++it;
                continue;
            }
            if (presenter.DeviceLost()) return false;
            const std::wstring id = it->first, why = it->second->failure();
            it = players.erase(it);
            unplayable.insert(id);
            if (id == track) {
                audio.FadeOut(0.2f);
                track.clear();
            }
            fallBack(id, why);
        }
        return true;
    };

    // Until every display has its wallpaper -- a video's first frame, an image read -- the still
    // stays up (or, without one, the windows stay hidden), so the change from Windows'
    // background -- that same picture, baked -- to the wallpaper is not a change at all.
    for (const ULONGLONG until = GetTickCount64() + kFirstFrameWaitMs; !done;) {
        Beat();
        if (!replaceFailed()) {
            ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
            done = true;
            break;
        }
        bool ready = true, changed = false;
        const std::vector<Presenter::Picture> pictures = compose(0.0, &ready, &changed, nullptr);
        if (ready || GetTickCount64() >= until) {
            presenter.Render(pictures, 0.0f, 1.0f);
            rendered(pictures);
            break;
        }
        Sleep(5);
    }
    Beat();
    if (!done) {
        presenter.Show();
        g_live.store(true);
        ALOG(L"overlay: live on %zu display(s), %llu ms after the logon screen", targets.size(),
             GetTickCount64() - g_overlay.seenAt);
    }

    // --- frame loop --------------------------------------------------------------------
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const ULONGLONG start = GetTickCount64();
    bool componentsFading = withComponents;
    // The video's clock stands still while the panel is dark or the machine sleeps.
    const ULONGLONG clockStart = screen::AwakeMs();
    ULONGLONG darkMs = 0, darkSince = 0;
    bool wasOn = true;
    int awayTicks = 0;
    // What reached the glass on the first display, for the log.
    int shownFrames = 0;
    double firstT = -1.0, lastT = 0.0;
    ULONGLONG lastExitCheck = 0;
    ULONGLONG lastKeepTop = 0;

    while (!done) {
        double period = 1.0 / 30.0;
        for (auto &[id, player] : players) period = std::min(period, player->frameSeconds());
        period = std::max(period, 1.0 / 120.0);
        if (g_overlay.woke) period = std::min(period, refresh);
        // Wake just after the compositor's next frame, so the picture drawn then is ready for
        // the one after; a fixed period drifts against the display and drops frames.
        double wait = period;
        if (presenter.SecondsToNextComposition(&wait)) wait += kAfterComposeSeconds;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(wait * 1e7);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        Beat();
        if (!PumpMessages()) {  // WM_QUIT: rescue hotkey or shutdown
            end = LiveEnd::Leave;
            break;
        }

        const ULONGLONG now = GetTickCount64();

        if (inputsink::Drain(g_overlay.inputWindow, g_overlay.intent) || g_overlay.intent.requested()) Wake(L"a press seen by raw input");

        if (g_overlay.displayOn == false) {
            LASTINPUTINFO li{sizeof(li), 0};
            if (GetLastInputInfo(&li) && (LONG)(li.dwTime - g_overlay.displayOffInput) > 0) {
                g_overlay.displayOn = true;
                keyhook::State().displayOff.store(false);
            }
        }
        const bool draw = g_overlay.displayOn || g_overlay.woke;
        bool lit = false;
        if (draw != wasOn) {
            wasOn = draw;
            lit = draw;
            if (draw) {
                darkMs += screen::AwakeMs() - darkSince;
                audio.Resume();
            } else {
                darkSince = screen::AwakeMs();
                audio.Pause();
            }
        }

        audio.Tick();
        const bool componentsChanged = withComponents && layer.Tick();

        double t = 0.0;
        if (!audio.TrackClock(&t)) {
            const ULONGLONG dark = draw ? darkMs : darkMs + (screen::AwakeMs() - darkSince);
            t = (double)(screen::AwakeMs() - clockStart - dark) / 1000.0;
        }

        float opacity = 1.0f;
        if (g_overlay.woke) {
            const float k = (float)(now - g_overlay.fadeStart) / 1000.0f / kFadeSeconds;
            opacity = k >= 1.0f ? 0.0f : 1.0f - k;
            if (k >= 1.0f) {  // faded out: hand the screen back and park
                done = true;
                end = LiveEnd::Dismissed;
            }
        }

        // A video that stopped decoding is replaced before this tick's picture is composed, so
        // the built-in wallpaper is drawn in its place at once.
        if (!done && !replaceFailed()) {
            ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
            end = LiveEnd::Leave;
            break;
        }

        if (draw) {
            // Drawn only when something on it changed: a still wallpaper costs nothing between
            // the minutes, and a high refresh rate need not cost a full-screen draw per tick.
            bool fresh = lit || componentsChanged || g_overlay.woke || componentsFading;
            if (componentsFading) {
                const float k = std::min(1.0f, (float)(now - start) / 1000.0f / kClockFadeInSeconds);
                componentsFading = k < 1.0f;
                presenter.SetComponentOpacity(k * k * (3.0f - 2.0f * k));
            }
            bool ready = true, changed = false, firstFresh = false;
            const std::vector<Presenter::Picture> pictures = compose(t, &ready, &changed, &firstFresh);
            if (firstFresh) {
                ++shownFrames;
                if (firstT < 0) firstT = t;
                lastT = t;
            }
            fresh = fresh || changed;
            if (fresh) {
                if (presenter.Render(pictures, 0.0f, opacity)) {
                    rendered(pictures);
                } else if (presenter.DeviceLost()) {
                    ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
                    end = LiveEnd::Leave;
                    break;
                }
            }
        }

        if (now - lastKeepTop > 500) {
            lastKeepTop = now;
            presenter.KeepOnTop();
        }

        // Leave when the logon screen does (real mode), after three readings.
        if (!opt.windowed && now - lastExitCheck > 500) {
            lastExitCheck = now;
            placeComponents();
            const screen::Session s = screen::ReadSession(g_overlay.session);
            const bool stillLogon = s.console && (!s.signedIn || s.locked);
            if (!screen::InputDesktopIsSecure() && !stillLogon) {
                if (++awayTicks >= 3) {
                    ALOG(L"overlay: the logon screen is gone");
                    end = LiveEnd::LogonGone;
                    break;
                }
            } else {
                awayTicks = 0;
            }
            if (events::IsSet(events::kStop) || animelogon::machine::Paused()) {
                g_overlay.leaving = true;
                end = LiveEnd::Leave;
                break;
            }
        }

        // The windows were laid out for displays that are no longer there.
        if (g_overlay.displaysChanged && !g_overlay.woke && !opt.windowed) {
            ALOG(L"overlay: the displays changed -- laying the screen out again");
            end = LiveEnd::Rebuild;
            break;
        }

        if (opt.seconds && now - start > (ULONGLONG)opt.seconds * 1000) {
            end = LiveEnd::Leave;
            break;
        }
    }

    if (shownFrames > 1 && lastT > firstT) {
        const auto it = players.find(targets.front().wallpaper.id);
        const double videoFps = it != players.end() ? 1.0 / it->second->frameSeconds() : 0.0;
        ALOG(L"overlay: %d frames in %.1f s of video, %.1f fps shown of %.1f; the compositor runs at %.1f Hz",
             shownFrames, lastT - firstT, (shownFrames - 1) / (lastT - firstT), videoFps,
             presenter.CompositionRate());
    }

    // --- tear the appearance down ------------------------------------------------------
    presenter.Hide();
    g_live.store(false);
    if (power) UnregisterPowerSettingNotification(power);
    if (g_overlay.keyhookOn) {
        keyhook::Uninstall();
        ALOG(L"keyhook: %ld key event(s) withheld this time", keyhook::State().withheld.exchange(0));
    }
    UnregisterHotKey(g_overlay.inputWindow, kRescueHotkeyId);
    inputsink::Unregister();
    audio.FadeOut(0.2f);
    for (int i = 0; i < 12; ++i) {
        audio.Tick();
        Sleep(16);
    }
    players.clear();
    presenter.DestroyWindows();
    g_overlay.inputWindow = nullptr;
    if (timer) CloseHandle(timer);
    return end;
}

// Ends the process if a live screen's loop stops turning. The launcher then starts a parked
// overlay, and Windows' own screen is back within a few seconds.
void StartWatchdog() {
    std::thread([] {
        for (;;) {
            Sleep(500);
            if (g_live.load() && GetTickCount64() - g_heartbeat.load() > kHangMs)
                TerminateProcess(GetCurrentProcess(), kExitDismissed);
        }
    }).detach();
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    const Options opt = ParseOptions(argc, argv);
    g_still = opt.windowed && opt.still;
    if (!opt.bakePreview.empty()) {
        DeclareDpi();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        const bool ok = bake::Refresh(opt.bakePreview);
        MFShutdown();
        return ok ? 0 : 1;
    }
    animelogon::log::Open(animelogon::paths::LogPath(L"overlay.log"));
    DeclareDpi();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT mfStart = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfStart)) ALOG(L"overlay: MFStartup failed (0x%08X)", mfStart);
    ProcessIdToSessionId(GetCurrentProcessId(), &g_overlay.session);

    animelogon::instance::Guard guard;
    if (!opt.windowed && guard.Acquire(animelogon::instance::OverlayName(), true) != animelogon::instance::Result::Acquired) {
        ALOG(L"overlay: another overlay already owns this desktop -- exiting");
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // Session lock and unlock notifications arrive at a window that is never shown.
    HWND notify = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!opt.windowed && (!notify || !WTSRegisterSessionNotification(notify, NOTIFY_FOR_THIS_SESSION)))
        ALOG(L"overlay: no session notifications (%lu) -- relying on the session state", GetLastError());

    Presenter presenter;
    if (!presenter.CreateDevice()) return 2;
    if (!opt.windowed) LoadStill(presenter);
    g_overlay.seenAt = GetTickCount64();
    StartWatchdog();

    // Park, appear, hand back, park again. Once dismissed, the parked wait holds the video off
    // the screen until the session is used and locked afresh.
    bool dismissed = opt.dismissed;
    for (;;) {
        if (!opt.windowed && !opt.now && !WaitForLogonScreen(presenter, &dismissed)) break;

        LiveEnd end = GoLiveOnce(presenter, opt, wc.hInstance);
        while (end == LiveEnd::Rebuild) {
            Sleep(300);  // let the mode change settle
            end = GoLiveOnce(presenter, opt, wc.hInstance);
        }

        if (opt.windowed || opt.now) break;  // development runs are one-shot
        if (end == LiveEnd::Dismissed) {
            dismissed = true;
            continue;
        }
        if (end == LiveEnd::LogonGone) {
            dismissed = false;
            continue;
        }
        // Leaving: a screen somebody already waved away must stay away in the next process.
        dismissed = dismissed || g_overlay.woke;
        break;
    }

    if (notify) {
        WTSUnRegisterSessionNotification(notify);
        DestroyWindow(notify);
    }
    MFShutdown();
    ALOG(L"overlay: gone");
    return dismissed ? kExitDismissed : 0;
}
