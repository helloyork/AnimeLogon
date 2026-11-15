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

#include <algorithm>
#include <atomic>
#include <cstdlib>
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
#include "animelogon/audiocapture.h"
#include "animelogon/settings.h"
#include "animelogon/spectrum.h"

#include "audio.h"
#include "bake.h"
#include "clocktext.h"
#include "componentlayer.h"
#include "datafeed.h"
#include "displaylayout.h"
#include "events.h"
#include "inputsink.h"
#include "keyhook.h"
#include "otherdisplays.h"
#include "overlayexit.h"
#include "parkedplan.h"
#include "plan.h"
#include "player.h"
#include "presenter.h"
#include "scenes.h"
#include "staticcover.h"
#include "screen.h"
#include "wake.h"
#include "watchdog.h"

namespace {

constexpr wchar_t kWindowClass[] = L"AnimeLogonOverlay";
constexpr int kRescueHotkeyId = 0xA10E;  // Ctrl+Alt+F11
constexpr float kFadeSeconds = 0.5f;
// How long after the compositor's frame the next picture is drawn: right after one frame
// leaves nearly a whole frame of slack for the next, which a late wake-up would not.
constexpr double kAfterComposeSeconds = 0.001;
// How often the frame loop turns while the display is off.
constexpr double kRestingSeconds = 0.25;
// Exit code telling the launcher to start the next overlay parked (see kDismissedFlag); the
// watchdog's too (overlayexit.h).
constexpr int kExitDismissed = (int)overlayexit::kDismissed;
constexpr wchar_t kDismissedFlag[] = L"--dismissed";
// How often a parked overlay looks at whether the sign-in background needs baking again.
constexpr ULONGLONG kBakeCheckMs = 5000;
// How often a parked overlay looks at whether settings.ini or the displays changed (parkedplan.h).
constexpr ULONGLONG kPlanCheckMs = 2000;
// How often a parked overlay away from the logon desktop reads its session's state.
constexpr ULONGLONG kSessionReadMs = 500;
// How long the windows stay hidden waiting for the first frame. Until they show, Windows'
// own background -- that same frame -- is what is on the screen.
constexpr ULONGLONG kFirstFrameWaitMs = 1500;
// A display change while the screen is up is acted on once the displays have held still this long:
// one change -- a monitor waking, a dock -- usually comes as several in a row.
constexpr ULONGLONG kSettleMs = 300;
// How often the frame loop compares the displays with the layout its windows were made for.
constexpr ULONGLONG kLayoutCheckMs = 500;

struct Options {
    bool windowed = false;   // run on the ordinary desktop, for development
    bool now = false;        // skip the parked wait for a logon screen
    bool dismissed = false;  // the previous overlay was waved away: wait for the screen to be used
    bool still = false;      // with --windowed: input does not wake it, for screenshots
    bool hadUser = false;    // the launcher has seen somebody signed in to this session
    int seconds = 0;         // exit after this many seconds, for testing; 0 is forever
    std::wstring bakePreview;  // write the sign-in background this would bake there, and exit
    std::wstring stillsPreview;  // write the other displays' stills into this directory, and exit
};

// Why a live appearance ended, and so what to do next.
enum class LiveEnd {
    Leave,      // the service is stopping, shutting down, paused, or the device was lost: exit
    Dismissed,  // somebody arrived (or there was nothing to show): park until the screen is used
    LogonGone,  // the logon screen went away on its own: park for the next lock
    Rebuild,    // the displays changed under it and could not be laid out in place: go live again
    NoDisplays, // no display to cover: park, not dismissed, until one comes back
};

struct Overlay {
    HWND inputWindow = nullptr;  // the primary window, which owns raw input and the hotkey
    wake::Intent intent;
    bool woke = false;
    bool keyhookOn = false;
    double fadeStart = 0.0;  // when it was woken, in PreciseSeconds()
    bool displayOn = true;
    ULONGLONG displayOffAt = 0;
    DWORD displayOffInput = 0;
    bool leaving = false;        // the service stopping or the rescue key asked us to go
    bool endingSession = false;  // WM_ENDSESSION for a shutdown arrived (see GoingDown)
    bool displaysChanged = false; // a resolution, scale or monitor change while live
    DWORD session = 0xFFFFFFFF;
    bool lockNotified = false;   // WTS_SESSION_LOCK seen, and no unlock or logon since
    bool hadUser = false;        // somebody has been signed in to this session
    ULONGLONG seenAt = 0;        // when the parked wait saw the logon screen
};

Overlay g_overlay;
// The displays other than the primary, from a wake until sign-in. It outlives an appearance:
// the stills stay up through the parked wait that follows a wake.
OtherDisplays g_otherDisplays;
const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

// The watchdog's view of the main thread (watchdog.h): whether it is timing it -- from the moment
// an appearance starts being prepared, while its screen is up, and while its videos are being
// closed -- when it last moved on, how long it may go without moving, and what it is doing.
// A main thread stuck anywhere in there could hold the password box hostage, or stop every later
// lock; the process is ended instead and Windows' own screen shows.
std::atomic<bool> g_watched{false};
std::atomic<ULONGLONG> g_heartbeat{0};
std::atomic<ULONGLONG> g_limit{watchdog::kLiveMs};
std::atomic<const wchar_t *> g_phase{L""};

void Beat() { g_heartbeat.store(GetTickCount64()); }

// Starts (or goes on) timing the main thread: `limit` without a beat while it is `phase`.
void Watch(ULONGLONG limit, const wchar_t *phase) {
    Beat();
    g_limit.store(limit);
    g_phase.store(phase);
    g_watched.store(true);
}

// Tells the launcher, once per change, whether this overlay is parked dismissed (events::kDismissed):
// it carries that over to the overlay of the next session after "Switch user".
void PublishDismissed(bool on) {
    static int said = -1;
    if (said == (int)on) return;
    if (!events::Signal(events::kDismissed, on)) return;  // no launcher: a development run
    said = on;
}

// Seconds on the performance counter, the clock the transitions are timed by. GetTickCount64
// moves in steps of 15.6 ms: timed by it, a transition stood still for two frames in three on a
// 180 Hz display and then jumped, which reads as a very low frame rate once the picture moves.
double PreciseSeconds() {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / frequency;
}

bool g_still = false;

// Windows is shutting down or restarting (events::kGoingDown, or this process's WM_ENDSESSION).
bool GoingDown() { return g_overlay.endingSession || events::IsSet(events::kGoingDown); }

// `how` says what woke it, for the log; which key is never recorded.
void Wake(const wchar_t *how) {
    if (g_overlay.woke || g_still) return;
    g_overlay.woke = true;
    g_overlay.fadeStart = PreciseSeconds();
    // A click wakes too; after any wake no further key is withheld.
    keyhook::State().armed.store(false);
    ALOG(L"wake: %s -- fading out", how);
}

// How long the wake transition takes: the primary display's theme's (plan.h), none for a cut.
float WakeSeconds(const plan::Plan &plan) {
    return plan.wake == animelogon::WakeStyle::Cut ? 0.0f : plan.wakeSeconds;
}

// How far it has gone at `at` (PreciseSeconds()): 0 at the wake, 1 once it is over. The screen's
// own transition and the other displays' stills (otherdisplays.h) both go by this.
float WakeProgress(const plan::Plan &plan, double at) {
    if (!g_overlay.woke) return 0.0f;
    const float seconds = WakeSeconds(plan);
    return seconds > 0.0f ? std::clamp((float)(at - g_overlay.fadeStart) / seconds, 0.0f, 1.0f) : 1.0f;
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
    case WM_POINTERDOWN: {
        // A pen or a finger touching the screen wakes it as a click does, on whichever of the
        // windows it touched; nothing is withheld. A pen hovering or coming into range sends no
        // WM_POINTERDOWN. The mouse is left to raw input: without EnableMouseInPointer it sends
        // no pointer messages, and a touchpad's clicks are mouse clicks.
        POINTER_INPUT_TYPE type = PT_POINTER;
        const bool penOrTouch = GetPointerType(GET_POINTERID_WPARAM(wp), &type) && (type == PT_PEN || type == PT_TOUCH);
        if (g_overlay.intent.ObservePointer(penOrTouch, (DWORD)GetMessageTime()))
            Wake(type == PT_PEN ? L"a pen touching the screen" : L"a touch on the screen");
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
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
        // Off the screen, and alive: an overlay that exited here would be replaced by the
        // launcher, and the replacement, finding nobody signed in on the logon desktop, would
        // cover Windows' shutdown or update screen. The flag tells every overlay, this one's
        // successors included, until the shutdown is over or visibly cancelled.
        if (wp && !(lp & ENDSESSION_LOGOFF)) {
            ALOG(L"shutdown: WM_ENDSESSION -- off the screen until Windows is back");
            g_overlay.endingSession = true;
            events::Signal(events::kGoingDown, true);
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
        else if (a == L"--had-user") o.hadUser = true;
        else if (a == L"--bake-preview" && i + 1 < argc) o.bakePreview = argv[++i];
        else if (a == L"--stills-preview" && i + 1 < argc) o.stillsPreview = argv[++i];
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
bool WaitForLogonScreen(Presenter &presenter, Scenes &scenes, bool *dismissed) {
    ULONGLONG bakedAt = 0;
    ULONGLONG planCheck = 0;
    parkedplan::Watch replan;
    screen::CancelWatch cancel;
    screen::Gate said = screen::Gate::Hold;  // the last reason to stay parked that was logged
    screen::Session s;
    ULONGLONG sessionAt = 0;
    for (;;) {
        Beat();
        if (events::IsSet(events::kStop) || animelogon::machine::Paused()) return false;
        if (!PumpMessages()) return false;
        // Scene wallpapers' renderers are made ready ahead of the lock: planned again only when
        // settings.ini or the displays change, and handed to the scenes only when there is a scene
        // to get ready (or one left from before to let go of).
        if (GetTickCount64() - planCheck > kPlanCheckMs) {
            planCheck = GetTickCount64();
            // A display mode change at the desktop can reset the graphics device. Found only at the
            // next lock, the fresh process the launcher then starts would take a moment to cover the
            // screen, and Windows' own sign-in would show meanwhile; ending now, away from any logon
            // screen, lets the new one be ready well before then.
            if (!screen::InputDesktopIsSecure() && presenter.DeviceLost()) {
                ALOG(L"overlay: the graphics device was lost while parked at the desktop -- exiting now so a fresh "
                     L"one is ready before the next lock");
                return false;
            }
            if (replan.Due(parkedplan::StampOf(animelogon::paths::SettingsPath()), displaylayout::Current(), planCheck)) {
                const plan::Plan next =
                    plan::Build(animelogon::LoadSettings(false), animelogon::EnumerateMonitors(), animelogon::DiskStore());
                const bool scene = std::any_of(next.displays.begin(), next.displays.end(),
                                               [](const plan::Display &d) { return d.wallpaper.source == plan::Source::Scene; });
                if (scene || replan.Scene()) scenes.Prepare(presenter, next);
                replan.Planned(scene);
            }
        }
        scenes.Tick({},false,60);
        const bool secure = screen::InputDesktopIsSecure();
        // On the logon desktop the session's state decides at once. Away from it, it only feeds
        // the slower checks below, so it is read twice a second rather than every turn.
        if (secure || GetTickCount64() - sessionAt >= kSessionReadMs) {
            s = screen::ReadSession(g_overlay.session);
            sessionAt = GetTickCount64();
        }
        if (!secure && s.console && s.signedIn && !s.locked && !g_overlay.lockNotified) scenes.Unlocked();
        if (s.signedIn) g_overlay.hadUser = true;

        // A shutdown that was cancelled leaves somebody at their unlocked desktop.
        const bool atDesktop = !secure && s.console && s.signedIn && !s.locked && !g_overlay.lockNotified;
        bool goingDown = GoingDown();
        if (cancel.Update(goingDown, atDesktop, GetTickCount64())) {
            ALOG(L"shutdown: somebody has been at an unlocked desktop for %llu s -- the shutdown was cancelled",
                 screen::CancelWatch::kCancelledMs / 1000);
            g_overlay.endingSession = false;
            events::Signal(events::kGoingDown, false);
            goingDown = false;
        }

        // The background is only ever rewritten while somebody is using the session.
        if (!secure && s.console && s.signedIn && !s.locked && !g_overlay.lockNotified &&
            GetTickCount64() - bakedAt > kBakeCheckMs) {
            bake::Refresh();
            LoadStill(presenter);
            bakedAt = GetTickCount64();
        }

        // A dismissed appearance stays dismissed until the screen is used again.
        if (*dismissed) {
            PublishDismissed(true);
            if (!secure || (s.signedIn && !s.locked && !g_overlay.lockNotified)) {
                *dismissed = false;
                PublishDismissed(false);
            } else {
                // Woken by any message too, so that a still whose display goes is dealt with at
                // once (otherdisplays.h), not after the rest of the wait.
                MsgWaitForMultipleObjectsEx(0, nullptr, g_otherDisplays.Parked() ? 16 : 200, QS_ALLINPUT,
                                            MWMO_INPUTAVAILABLE);
                continue;
            }
        }
        g_otherDisplays.Clear();  // the password screen is over
        PublishDismissed(false);

        screen::GateInput in;
        in.secure = secure;
        in.session = s;
        in.hadUser = g_overlay.hadUser;
        in.shutdown = screen::ShutdownUnderway();
        in.goingDown = goingDown;
        in.lockNotified = g_overlay.lockNotified;
        // A DisplayPort monitor that powers down leaves no display at all for a while.
        in.noDisplays = GetSystemMetrics(SM_CMONITORS) == 0;
        const screen::Gate gate = screen::Decide(in);
        if (gate == screen::Gate::GoLive) {
            g_overlay.seenAt = GetTickCount64();
            ALOG(L"overlay: logon screen (signed in %d, locked %d, lock notified %d, had a user %d, session state %d) "
                 L"over %s",
                 s.signedIn, s.locked, g_overlay.lockNotified, g_overlay.hadUser, s.state,
                 screen::DescribeDesktop().c_str());
            return true;
        }
        if (gate != said && gate != screen::Gate::Hold) {
            const wchar_t *why = gate == screen::Gate::GoingDown ? L"Windows is shutting down or restarting"
                                 : gate == screen::Gate::SignedOutInPlace
                                     ? L"this session's user was signed out in place (a shutdown or restart)"
                                 : gate == screen::Gate::NoDisplays
                                     ? L"no display is attached -- waiting for one"
                                     : L"the session is not at its logon screen (an update screen)";
            ALOG(L"overlay: the logon desktop is up but %s -- staying parked; session state %d; on it: %s", why,
                 s.state, screen::DescribeDesktop().c_str());
        }
        said = gate;
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

// The parked wait's gate, asked once: is the logon desktop up with a screen the video may cover?
bool LogonScreenNow() {
    screen::GateInput in;
    in.secure = screen::InputDesktopIsSecure();
    in.session = screen::ReadSession(g_overlay.session);
    in.hadUser = g_overlay.hadUser || in.session.signedIn;
    in.shutdown = screen::ShutdownUnderway();
    in.goingDown = GoingDown();
    in.lockNotified = g_overlay.lockNotified;
    return screen::Decide(in) == screen::Gate::GoLive;
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
LiveEnd GoLiveOnce(Presenter &presenter, Scenes &scenes, const Options &opt, HINSTANCE instance) {
    using Kind = Presenter::Picture::Kind;

    // Fresh state for this appearance.
    g_overlay.inputWindow = nullptr;
    g_overlay.woke = false;
    g_overlay.keyhookOn = false;
    g_overlay.fadeStart = 0.0;
    g_overlay.displayOn = true;
    g_overlay.displayOffAt = 0;
    g_overlay.displayOffInput = 0;
    g_overlay.leaving = false;
    g_overlay.displaysChanged = false;

    // Timed from here: covering the screen, opening the videos and setting up the components may
    // take a while, but a step that never returns -- a decoder that never hands over its first
    // frame -- must not leave the cover over the password box, nor the process stuck so that no
    // later lock shows anything. Untimed again however this returns.
    Watch(watchdog::kPrepareMs, L"preparing the screen");
    ALOG(L"watchdog: timing the preparation of the screen (%llu s without a step ends it)", watchdog::kPrepareMs / 1000);
    struct Unwatch {
        ~Unwatch() { g_watched.store(false); }
    } unwatch;

    // A device lost while parked is replaced by starting a fresh process.
    if (presenter.DeviceLost()) {
        scenes.Begin(); scenes.FailAll(L"graphics device was reset while preparing this lock"); scenes.End();
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
    // The displays the windows are made for, to notice a change that posts no WM_DISPLAYCHANGE.
    std::vector<RECT> madeFor = displaylayout::Current();
    if (opt.windowed) plan = WindowedPlan(std::move(plan));
    scenes.Prepare(presenter,plan);
    for (const std::wstring &p : plan.problems) ALOG(L"theme: %s", p.c_str());
    for (const plan::Display &d : plan.displays) ALOG(L"theme: %s", plan::Describe(d).c_str());

    // One window per display. `shown[i]` is what targets[i] came from.
    std::vector<Presenter::Target> targets;
    std::vector<const plan::Display *> shown;
    StaticCover staticCover;
    bool emergency = false;
    auto targetPlan = [&] {
        targets.clear();
        shown.clear();
        for (const plan::Display &d : plan.displays) {
            Presenter::Target t;
            t.rect = d.rect;
            t.canvas = d.canvas;
            t.wallpaper = d.wallpaper;
            t.showComponents = d.showComponents;
            targets.push_back(t);
            shown.push_back(&d);
        }
    };
    targetPlan();

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
            if ((w.source != plan::Source::Video && (w.source != plan::Source::Scene || w.videoPath.empty())) || players.count(w.id) || unplayable.count(w.id)) continue;
            if (w.source == plan::Source::Scene && scenes.Static(w.id)) continue;
            Beat();  // opening reads the first frame, which a stuck decoder may never hand over
            auto player = std::make_shared<VideoPlayer>();
            if (player->Open(w.source == plan::Source::Scene ? w.videoPath : w.path, presenter.device(), presenter.videoManager())) {
                players[w.id] = player;
            } else {
                ALOG(L"overlay: %s could not be opened", w.id.c_str());
                unplayable.insert(w.id);
            }
        }
    };
    auto replaceUnplayable = [&] {
        for (const std::wstring &id : unplayable) {
            const auto target = std::find_if(targets.begin(),targets.end(),[&](const auto &t) { return t.wallpaper.id == id; });
            if (target != targets.end() && target->wallpaper.source == plan::Source::Scene) scenes.Fail(id,L"video base could not be opened");
            else fallBack(id,L"it could not be opened");
        }
    };

    // Image wallpapers start reading at once, off this thread. One the device still holds from
    // an earlier appearance, its file unchanged, is ready now.
    auto requestImages = [&] {
        std::set<std::wstring> images;
        for (const Presenter::Target &t : targets)
            if ((t.wallpaper.source == plan::Source::Image || t.wallpaper.source == plan::Source::Scene) && images.insert(t.wallpaper.id).second)
                presenter.RequestImage(t.wallpaper.id, t.wallpaper.path);
        presenter.KeepImages(images);
    };
    requestImages();

    // The screen is covered first, so that Windows' credential screen is never seen. A
    // wallpaper that is ready at once -- an image already read, the built-in one, black -- is
    // the cover itself. Otherwise the baked still is: the picture Windows is already showing,
    // which the wallpaper replaces when it is ready. Without a still, nothing is shown until
    // there is a wallpaper to show.
    bool allReady = !targets.empty();
    for (const auto &target : targets) if (target.wallpaper.source == plan::Source::Scene)
        presenter.LoadImageNow(target.wallpaper.id,target.wallpaper.path);
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
        ALOG(L"overlay: no displays to cover -- parked until one comes back");
        return LiveEnd::NoDisplays;
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
            // The baked still is the primary display's picture: Windows shows it there alone, the
            // others stay black until their own wallpaper is ready.
            p.kind = still && shown[i]->primary ? Kind::Still : Kind::Black;
            const plan::Wallpaper &w = targets[i].wallpaper;
            switch (w.source) {
            case plan::Source::Scene: {
                p = scenes.Picture(w);
                const auto it = players.find(w.id);
                bool fresh = false;
                if (it != players.end() && it->second->FrameAt(t,&held[i],&fresh) && fresh)
                    scenes.Video(w.id,presenter,held[i],it->second->width(),it->second->height());
                break;
            }
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
    Beat();
    scenes.Begin();
    replaceUnplayable();

    // The components are drawn into the wallpaper's own frames, so they fade out with it. They
    // are set up after the cover and fade in once the wallpaper is up, so the cover never waits
    // for them. Instances drawn alike on several displays share one view, with a cache per window.
    ComponentLayer layer;
    DataFeed feed(g_overlay.session, settings.media);
    animelogon::data::Snapshot snapshot;
    bool withComponents = false;
    // Every instance set up so far, by key; kept when the displays change, so an instance on the
    // new layout that was on the old is the same view, already laid out and in.
    constexpr size_t kNone = (size_t)-1;
    std::map<std::wstring, size_t> byKey;
    int layerReady = 0;  // 1 the layer is up, -1 it could not be, 0 not tried
    const animelogon::RegionalFormat format = clocktext::UserFormat();
    auto setUpComponents = [&] {
        bool any = false;
        for (const plan::Display *d : shown) any = any || !d->components.empty();
        if (!any) return;
        if (!layerReady) layerReady = layer.Init(presenter.device()) ? 1 : -1;
        if (layerReady < 0) return;
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
        if (withComponents) presenter.SetComponents(&layer, perTarget);
    };
    Beat();
    setUpComponents();
    if (withComponents && feed.Poll(&snapshot)) layer.SetData(snapshot);
    if (withComponents) {
        presenter.SetComponentOpacity(0.0f);
        ALOG(L"components: %zu instance(s) set up", layer.size());
    }
    // By default the components go where Windows puts its password box, which LogonUI may not
    // have drawn yet; it is looked for again below until found.
    bool componentsPlaced = false;
    auto componentsStart = [&] {
        componentsPlaced =
            !withComponents || opt.windowed || plan.componentDisplays != animelogon::ComponentDisplays::Auto;
    };
    componentsStart();
    auto placeComponents = [&] {
        RECT monitor{};
        const wchar_t *why = L"";
        if (componentsPlaced || !screen::CredentialMonitor(&monitor, &why) || !presenter.MoveComponentsTo(monitor)) return;
        componentsPlaced = true;
        std::wstring key = L"?";
        for (const plan::Display *d : shown)
            if (EqualRect(&d->rect, &monitor)) key = d->monitorKey;
        ALOG(L"components: the password box is on display %s %ldx%ld (%s) -- the components are drawn there", key.c_str(),
             monitor.right - monitor.left, monitor.bottom - monitor.top, why);
    };
    placeComponents();

    // Without raw input nothing could wake the screen, so it is never covered.
    Beat();
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
    // Raw input, the rescue hotkey, the display's power state and the keyboard hook's word all go
    // to the primary window; when the windows are made again they move to the new one. A press
    // still queued for the old window is read first, so none is lost in between.
    auto bindInput = [&](HWND window) {
        if (!window || window == g_overlay.inputWindow) return;
        const HWND old = g_overlay.inputWindow;
        if (inputsink::Drain(old, g_overlay.intent)) Wake(L"a press seen by raw input");
        UnregisterHotKey(old, kRescueHotkeyId);
        if (power) UnregisterPowerSettingNotification(power);
        g_overlay.inputWindow = window;
        if (!inputsink::Register(window)) ALOG(L"overlay: raw input could not be moved to the new window (%lu)", GetLastError());
        if (!RegisterHotKey(window, kRescueHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11))
            ALOG(L"overlay: the rescue hotkey could not be moved to the new window (%lu)", GetLastError());
        keyhook::State().notify.store(window);
        power = RegisterPowerSettingNotification(window, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    };

    if (cover) {
        openPlayers();
        replaceUnplayable();
    }
    bool done = false;
    LiveEnd end = LiveEnd::Leave;
    Beat();

    // The audio configuration is global (the audio system's own); the only source a theme has
    // is its wallpaper's sound track, when the wallpaper is a video with one.
    AudioSystem audio;
    audio.Open(settings.audio);
    std::wstring track;  // the wallpaper whose sound track plays, if any

    // A spectrum listens to the output device while the screen is up, and is drawn every frame
    // while its bands move (animelogon/audiocapture.h). It listens to the signed-in person's output.
    const bool listening = withComponents && layer.Listens();
    animelogon::AudioCapture capture;
    animelogon::spectrum::Analyzer analyzer;
    animelogon::spectrum::Frame heard;
    std::vector<float> samples, ownSound;
    bool readingTrack = false;  // said once in the log
    if (listening) capture.Start(AudioSystem::ListeningDevice);
    const double refresh = RefreshSeconds();
    Beat();
    if (!opt.windowed && !done) {
        AudioSystem::RememberConsoleDefault();
        const plan::Wallpaper &first = targets.front().wallpaper;
        if ((first.source == plan::Source::Video || (first.source == plan::Source::Scene && !scenes.Static(first.id))) && !first.audioPath.empty()) {
            // Looped at the video's own length, so that sound and picture stay together.
            const auto player = players.find(first.id);
            const double loop = player != players.end() ? player->second->durationSeconds() : 0.0;
            audio.PlayVideoTrack(first.audioPath, 0.0, kFadeSeconds, loop);
            track = first.id;
        }
    }

    // A video that stops decoding gives way to the built-in wallpaper, and its sound track
    // stops with it. Returns false when the cause was a lost device, which ends the appearance.
    auto replaceFailed = [&] {
        if (emergency) return true;
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
            const auto target = std::find_if(targets.begin(),targets.end(),[&](const auto &t) { return t.wallpaper.id == id; });
            if (target != targets.end() && target->wallpaper.source == plan::Source::Scene) scenes.Fail(id,why);
            else fallBack(id, why);
        }
        return true;
    };

    auto staticAfterReset = [&] {
        bool scene = false;
        for (const auto &target : targets) scene = scene || target.wallpaper.source == plan::Source::Scene;
        if (!scene) return false;
        scenes.FailAll(L"graphics device was reset");
        std::vector<Presenter::Target> cpuTargets;
        const auto monitors = animelogon::EnumerateMonitors();
        const RECT wall = animelogon::VirtualBounds(monitors);
        for (size_t i = 0; i < monitors.size(); ++i) {
            auto target = targets[std::min(i,targets.size()-1)];
            target.rect = monitors[i].rect;
            target.canvas = settings.monitorMode == animelogon::MonitorMode::Span ? wall : target.rect;
            cpuTargets.push_back(std::move(target));
        }
        if (!staticCover.Show(cpuTargets.empty() ? targets : cpuTargets,WndProc)) return false;
        presenter.Hide();
        UnregisterHotKey(g_overlay.inputWindow,kRescueHotkeyId);
        if (power) UnregisterPowerSettingNotification(power);
        g_overlay.inputWindow = staticCover.primary();
        inputsink::Register(g_overlay.inputWindow);
        RegisterHotKey(g_overlay.inputWindow,kRescueHotkeyId,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F11);
        keyhook::State().notify.store(g_overlay.inputWindow);
        power = RegisterPowerSettingNotification(g_overlay.inputWindow,&kConsoleDisplayState,DEVICE_NOTIFY_WINDOW_HANDLE);
        audio.FadeOut(.2f); emergency = true;
        g_overlay.displaysChanged = false;
        ALOG(L"scene: graphics reset -- CPU static cover is up; input and fade remain active");
        return true;
    };
    if (presenter.DeviceLost() && !staticAfterReset()) {
        ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
        done = true;
    }

    // Until every display has its wallpaper -- a video's first frame, an image read -- the still
    // stays up (or, without one, the windows stay hidden), so the change from Windows'
    // background -- that same picture, baked -- to the wallpaper is not a change at all.
    for (const ULONGLONG until = GetTickCount64() + kFirstFrameWaitMs; !done && !emergency;) {
        Beat();
        if (!replaceFailed()) {
            if (staticAfterReset()) break;
            ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
            done = true;
            break;
        }
        bool ready = true, changed = false;
        scenes.Tick({},true,(float)(1/refresh));
        const std::vector<Presenter::Picture> pictures = compose(0.0, &ready, &changed, nullptr);
        if (ready || GetTickCount64() >= until) {
            if (!presenter.Render(pictures, 0.0f, 1.0f) && presenter.DeviceLost() && !staticAfterReset()) done = true;
            rendered(pictures);
            break;
        }
        Sleep(5);
    }
    Beat();
    if (!done) {
        if (!emergency) presenter.Show();
        Watch(watchdog::kLiveMs, L"showing the screen");
        ALOG(L"overlay: live on %zu display(s), %llu ms after the logon screen; the watchdog allows the frame loop %llu s",
             targets.size(), GetTickCount64() - g_overlay.seenAt, watchdog::kLiveMs / 1000);
    }
    if (!done && !opt.windowed) g_otherDisplays.Prepare(plan, settings.otherDisplays, WakeSeconds(plan));

    // --- frame loop --------------------------------------------------------------------
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const ULONGLONG start = GetTickCount64();
    const double entranceStart = PreciseSeconds();  // what the components' entrances count from
    bool componentsFading = withComponents;
    // The video's clock stands still while the panel is dark or the machine sleeps.
    const ULONGLONG clockStart = screen::AwakeMs();
    ULONGLONG darkMs = 0, darkSince = 0;
    bool wasOn = true;
    int awayTicks = 0;
    int notLogonTicks = 0;  // exit checks in a row with the session not at its logon screen
    // When the displays last changed under the windows, to lay the screen out again once they
    // hold still (0: no change waiting), and when the layout was last compared.
    ULONGLONG relayoutAt = 0, lastLayoutCheck = 0;

    // Lays the screen out again for the displays as they are now, without uncovering it even for a
    // frame: the new windows are made, drawn and shown over the old ones, which go only then. The
    // keyboard hook stays in, the videos keep playing where they were and the sound track with
    // them. `t` is the videos' clock. Fallback when only the long way will do: tearing the
    // appearance down and going live afresh, as a layout with a scene wallpaper needs.
    enum class Relaid { Done, NoDisplays, Fallback, DeviceLost };
    auto relayOut = [&](double t) -> Relaid {
        const ULONGLONG began = GetTickCount64();
        Beat();
        std::wstring why;
        const animelogon::Settings fresh = animelogon::LoadSettings(false, &why);
        plan::Plan next = plan::Build(fresh, animelogon::EnumerateMonitors(), animelogon::DiskStore());
        madeFor = displaylayout::Current();
        if (next.displays.empty()) return Relaid::NoDisplays;
        auto anyScene = [](const plan::Plan &p) {
            return std::any_of(p.displays.begin(), p.displays.end(),
                               [](const plan::Display &d) { return d.wallpaper.source == plan::Source::Scene; });
        };
        if (anyScene(plan) || anyScene(next)) return Relaid::Fallback;
        ALOG(L"overlay: the displays changed -- laying the screen out again in place: %zu display(s), %zu before",
             next.displays.size(), targets.size());
        for (const std::wstring &p : next.problems) ALOG(L"theme: %s", p.c_str());
        for (const plan::Display &d : next.displays) ALOG(L"theme: %s", plan::Describe(d).c_str());
        plan = std::move(next);
        targetPlan();
        requestImages();
        // The videos still shown play on, new ones open, the rest close (closing waits for their
        // decoding threads: a beat first).
        openPlayers();
        for (auto it = players.begin(); it != players.end();) {
            const bool used = std::any_of(targets.begin(), targets.end(),
                                          [&](const Presenter::Target &x) { return x.wallpaper.id == it->first; });
            if (used) {
                ++it;
                continue;
            }
            if (it->first == track) {
                audio.FadeOut(0.2f);
                track.clear();
                ALOG(L"overlay: the sound track's wallpaper is on no display now -- its sound fades out");
            }
            Beat();
            it = players.erase(it);
        }
        held.assign(targets.size(), VideoPlayer::Frame{});
        drawn.assign(targets.size(), Kind::Black);
        Beat();
        if (!presenter.CreateWindows(targets, kWindowClass, instance, true)) {
            if (presenter.DeviceLost()) return Relaid::DeviceLost;
            ALOG(L"overlay: the windows for the new layout could not be made");
            return Relaid::Fallback;
        }
        replaceUnplayable();
        bindInput(presenter.primary());
        setUpComponents();
        // Instances new to this layout come in at once, as those already up did long ago.
        componentsFading = withComponents;
        componentsStart();
        placeComponents();
        bool ready = true, changed = false;
        const std::vector<Presenter::Picture> pictures = compose(t, &ready, &changed, nullptr);
        if (!presenter.Render(pictures, 0.0f, 1.0f) && presenter.DeviceLost()) return Relaid::DeviceLost;
        rendered(pictures);
        presenter.Show();
        // The new windows reach the glass before the old ones go: a couple of the compositor's
        // frames. Anything posted to the old windows meanwhile -- the keyboard hook's wake -- is
        // handled before they are destroyed.
        Sleep((DWORD)std::min(50.0, 2000.0 * refresh) + 1);
        Beat();
        if (!PumpMessages()) PostQuitMessage(0);
        presenter.RetireOld();
        g_otherDisplays.Prepare(plan, fresh.otherDisplays, WakeSeconds(plan));
        g_overlay.displaysChanged = false;
        ALOG(L"overlay: laid out again in place on %zu display(s) in %llu ms -- the screen stayed covered", targets.size(),
             GetTickCount64() - began);
        return Relaid::Done;
    };
    // What reached the glass on the first display, for the log.
    int shownFrames = 0;
    double firstT = -1.0, lastT = 0.0;
    ULONGLONG lastExitCheck = 0;
    ULONGLONG lastKeepTop = 0;
    // How the wake transition went, for the log: the frames drawn, how many of them moved it on,
    // and the longest wait between two of them.
    int wakeFrames = 0, wakeSteps = 0;
    float wakeK = 0.0f, wakeDrawnK = -1.0f;
    double wakeDrawnAt = 0.0, wakeGap = 0.0;

    while (!done) {
        double period = 1.0 / 30.0;
        for (auto &[id, player] : players) period = std::min(period, player->frameSeconds());
        period = std::max(period, 1.0 / 120.0);
        if (g_overlay.woke) period = std::min(period, refresh);
        // A spectrum moves at the display's rate, up to 60 frames a second.
        if (listening && !emergency) period = std::min(period, std::max(refresh, 1.0 / 60.0));
        // Wake just after the compositor's next frame, so the picture drawn then is ready for
        // the one after; a fixed period drifts against the display and drops frames.
        // With the display off nothing is drawn: the loop only looks around a few times a second.
        // A press, the display coming back on, anything posted, wakes it at once all the same.
        const bool resting = !g_overlay.displayOn && !g_overlay.woke;
        if (resting) period = kRestingSeconds;
        double wait = period;
        if (!emergency && !resting && presenter.SecondsToNextComposition(&wait)) wait += kAfterComposeSeconds;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(wait * 1e7);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        Beat();
        if (!PumpMessages()) {  // WM_QUIT: rescue hotkey or shutdown
            end = LiveEnd::Leave;
            break;
        }
        // A mode change may remove the device before the next Present or decoder error.
        if (!emergency && presenter.DeviceLost() && !staticAfterReset()) {
            end = LiveEnd::Leave;
            break;
        }

        const ULONGLONG now = GetTickCount64();
        // The moment the picture drawn on this turn is first seen: the compositor's next frame.
        // Transitions are drawn for that moment, so a movement steps on by one whole frame each
        // time whatever the timer's own jitter.
        double ahead = 0.0;
        const double shownAt = PreciseSeconds() + (!emergency && presenter.SecondsToNextComposition(&ahead) ? ahead : 0.0);

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

        // The video's own clock, standing still while the panel is dark. A sound track that starts
        // late -- an output device that appears seconds into the boot screen -- starts from here.
        const ULONGLONG dark = draw ? darkMs : darkMs + (screen::AwakeMs() - darkSince);
        const double pictureClock = (double)(screen::AwakeMs() - clockStart - dark) / 1000.0;
        audio.Tick(pictureClock);
        if (!emergency && feed.Poll(&snapshot) && withComponents) layer.SetData(snapshot);
        const bool componentsChanged = withComponents && !emergency && layer.Tick();
        // The spectra take the latest sound once a tick while the screen is lit; until the device
        // is open they hear silence. Drawn again only while their bands move.
        bool listened = false;
        if (listening && draw && !emergency) {
            float rate = 0.0f, ownRate = 0.0f;
            const bool device = capture.Latest(&samples, &rate);
            // Before anybody has signed in since the machine started, Windows lets the logon screen's
            // own sound track through to the speakers but hands a listener only silence. When the
            // device gives much less than the track plays, the spectrum reads the track itself.
            auto loudness = [](const std::vector<float> &v) {
                double sum = 0.0;
                for (float x : v) sum += (double)x * x;
                return v.empty() ? 0.0 : sum / (double)v.size();
            };
            const bool own = audio.Recent(&ownSound, animelogon::spectrum::Analyzer::SizeFor(48000.0f), &ownRate) &&
                             (!device || loudness(samples) < 0.25 * loudness(ownSound));
            if (own) {
                if (!readingTrack) ALOG(L"spectrum: the device gives a listener less than the sound track plays -- reading the track itself");
                readingTrack = true;
                analyzer.Run(ownSound.data(), ownSound.size(), ownRate, now / 1000.0, &heard);
            } else if (device) {
                analyzer.Run(samples.data(), samples.size(), rate, now / 1000.0, &heard);
            } else {
                heard = {{}, 0.0f, false, now / 1000.0};
            }
            layer.SetAudio(heard);
            listened = layer.Moving();
        }

        // Once its sound track plays, the video follows the track's clock instead.
        double t = pictureClock;
        audio.TrackClock(&t);

        // Woken: the screen gives way to the sign-in screen as the primary display's theme says
        // (plan.h): fading, growing as it fades, sliding up off the display, or at once.
        float opacity = 1.0f, leaveScale = 1.0f, leaveRise = 0.0f;
        if (g_overlay.woke) {
            const float k = wakeK = WakeProgress(plan, shownAt);
            const float c = std::min(k, 1.0f), smooth = c * c * (3.0f - 2.0f * c);
            switch (plan.wake) {
            case animelogon::WakeStyle::Zoom:
                opacity = 1.0f - smooth;
                leaveScale = 1.0f + 0.08f * (1.0f - (1.0f - c) * (1.0f - c) * (1.0f - c));
                break;
            case animelogon::WakeStyle::Slide: leaveRise = -smooth; break;
            case animelogon::WakeStyle::Cut: opacity = 0.0f; break;
            default: opacity = 1.0f - c; break;
            }
            if (k >= 1.0f) {  // gone: hand the screen back and park
                opacity = 0.0f;
                done = true;
                end = LiveEnd::Dismissed;
            }
            presenter.SetLeave(leaveScale, leaveRise);
        }
        if (!opt.windowed) g_otherDisplays.Live(g_overlay.woke, WakeProgress(plan, shownAt));

        // A video that stopped decoding is replaced before this tick's picture is composed, so
        // the built-in wallpaper is drawn in its place at once.
        if (!done && !replaceFailed()) {
            if (staticAfterReset()) continue;
            ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
            end = LiveEnd::Leave;
            break;
        }

        if (draw && emergency) {
            staticCover.Opacity(opacity);
        } else if (draw) {
            // Drawn only when something on it changed: a still wallpaper costs nothing between
            // the minutes, and a high refresh rate need not cost a full-screen draw per tick.
            bool fresh = lit || componentsChanged || g_overlay.woke || componentsFading || listened;
            animelogon::scene::Inputs sceneInputs; sceneInputs.time = (float)t;
            SYSTEMTIME local{}; GetLocalTime(&local);
            sceneInputs.day = (local.wHour*3600.f+local.wMinute*60.f+local.wSecond)/86400.f;
            sceneInputs.battery = snapshot.battery.percent < 0 ? -1.f : snapshot.battery.percent/100.f;
            sceneInputs.playing = (float)snapshot.media.state;
            animelogon::data::Context dataContext; dataContext.snapshot = &snapshot;
            FILETIME utc{}; GetSystemTimeAsFileTime(&utc);
            dataContext.now = ((uint64_t)utc.dwHighDateTime<<32)|utc.dwLowDateTime;
            double progress = 0; animelogon::data::FractionOf(L"media.progress",dataContext,&progress);
            sceneInputs.progress = (float)progress;
            fresh = scenes.Tick(sceneInputs,draw,(float)(1/refresh)) || fresh;
            if (!track.empty() && scenes.Static(track)) { audio.FadeOut(.2f); track.clear(); }
            if (componentsFading) {
                // Each panel comes in as its component says (enter, enter-delay, enter-duration),
                // counted from the first tick with the wallpaper up.
                layer.SetEntrance((float)(shownAt - entranceStart));
                componentsFading = layer.Entering();
                presenter.SetComponentOpacity(1.0f);
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
                    if (g_overlay.woke) {
                        const double at = PreciseSeconds();
                        ++wakeFrames;
                        if (wakeK != wakeDrawnK) ++wakeSteps;
                        if (wakeDrawnAt > 0.0) wakeGap = std::max(wakeGap, at - wakeDrawnAt);
                        wakeDrawnK = wakeK;
                        wakeDrawnAt = at;
                    }
                } else if (presenter.DeviceLost()) {
                    if (staticAfterReset()) continue;
                    ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
                    end = LiveEnd::Leave;
                    break;
                }
            }
        }
        if (!draw) scenes.Tick({},false,(float)(1/refresh));

        if (now - lastKeepTop > 500) {
            lastKeepTop = now;
            if (emergency) staticCover.KeepOnTop(); else presenter.KeepOnTop();
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
            // A restart from a live lock or logon screen -- Windows Update's overnight restart, or
            // "Update and restart" from the power button -- keeps this desktop and this session,
            // so nothing above sees the screen go. Dismissed rather than left: the process stays,
            // so the launcher has nothing to replace.
            if (GoingDown()) {
                ALOG(L"overlay: Windows is shutting down or restarting -- off the screen");
                end = LiveEnd::Dismissed;
                break;
            }
            if (g_overlay.hadUser && s.console && !s.signedIn) {
                ALOG(L"overlay: this session's user was signed out in place (a shutdown or restart) -- off the screen");
                end = LiveEnd::Dismissed;
                break;
            }
            // Two readings a second apart, so a passing state never takes the video away.
            notLogonTicks = s.AtLogonStage() ? 0 : notLogonTicks + 1;
            if (notLogonTicks >= 2) {
                ALOG(L"overlay: the session left its logon screen (state %d) -- off the screen", s.state);
                end = LiveEnd::Dismissed;
                break;
            }
        }

        // The windows were laid out for displays that are no longer there.
        if (g_overlay.displaysChanged && emergency && !g_overlay.woke && !opt.windowed) {
            if (!staticAfterReset()) { end = LiveEnd::Leave; break; }
            continue;
        }
        // A change is seen by WM_DISPLAYCHANGE or, since some post none, by comparing the layout
        // every half second; it is acted on once the displays have held still for a moment.
        if (!emergency && !g_overlay.woke && !opt.windowed) {
            if (now - lastLayoutCheck >= kLayoutCheckMs) {
                lastLayoutCheck = now;
                if (!displaylayout::Same(displaylayout::Current(), madeFor)) g_overlay.displaysChanged = true;
            }
            if (g_overlay.displaysChanged) {
                g_overlay.displaysChanged = false;
                if (!relayoutAt) ALOG(L"overlay: the displays are changing -- the screen stays covered");
                relayoutAt = now + kSettleMs;
            }
        }
        if (relayoutAt && now >= relayoutAt && !emergency && !g_overlay.woke && !opt.windowed) {
            relayoutAt = 0;
            // Covering the new layout passes the same gate as going live: a display change can come
            // in the middle of a shutdown (drivers unloading). No display at all is waited out.
            if (GetSystemMetrics(SM_CMONITORS) > 0 && !LogonScreenNow()) {
                ALOG(L"overlay: the displays changed, and this is no longer a screen to cover -- off the screen");
                end = LiveEnd::Dismissed;
                break;
            }
            const Relaid relaid = relayOut(t);
            if (relaid == Relaid::NoDisplays) {
                ALOG(L"overlay: the displays changed and none is attached now -- the screen stays as it is until one is");
            } else if (relaid == Relaid::DeviceLost) {
                ALOG(L"overlay: the graphics device was lost laying the screen out again -- exiting so a fresh one starts");
                end = LiveEnd::Leave;
                break;
            } else if (relaid == Relaid::Fallback) {
                ALOG(L"overlay: the displays changed -- laying the screen out again from the start");
                end = LiveEnd::Rebuild;
                break;
            }
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
    if (wakeFrames > 1)
        ALOG(L"wake: %s over %.2f s -- %d frames drawn, %d of them moved it on, longest gap %.1f ms",
             animelogon::ToString(plan.wake), WakeSeconds(plan), wakeFrames, wakeSteps, wakeGap * 1000.0);

    // --- tear the appearance down ------------------------------------------------------
    scenes.End();
    presenter.Hide();
    g_watched.store(false);
    // Somebody waved the screen away: the launcher is told, for "Switch user" (overlayexit.h).
    if (g_overlay.woke) PublishDismissed(true);
    if (power) UnregisterPowerSettingNotification(power);
    if (g_overlay.keyhookOn) {
        keyhook::Uninstall();
        ALOG(L"keyhook: %ld key event(s) withheld this time", keyhook::State().withheld.exchange(0));
    }
    UnregisterHotKey(g_overlay.inputWindow, kRescueHotkeyId);
    inputsink::Unregister();
    capture.Stop();
    audio.FadeOut(0.2f);
    for (int i = 0; i < 12; ++i) {
        audio.Tick(0.0);  // fading out: nothing is waiting to start
        Sleep(16);
    }
    // Closing a video waits for its decoding thread, and a decoder that stopped handing out frames
    // without failing may never let that thread go. The process would then sit here, alive, and
    // the launcher would start no other, so no later lock would show anything. Under the watchdog
    // it ends instead, and the launcher starts a parked overlay in its place.
    Watch(watchdog::kLiveMs, L"closing the videos");
    players.clear();
    g_watched.store(false);
    presenter.DestroyWindows();
    g_overlay.inputWindow = nullptr;
    if (timer) CloseHandle(timer);
    return end;
}

// The watchdog's last words, from a thread of its own: the main thread may be stuck holding
// whatever writing a line needs, and the process is ended whether or not this got through.
DWORD WINAPI SayStuck(void *) {
    const ULONGLONG now = GetTickCount64(), beat = g_heartbeat.load();
    animelogon::log::TryWrite(L"watchdog: nothing has moved for %llu ms while %s (%llu s allowed) -- ending the process "
                              L"with exit code %d, so Windows' sign-in shows and the next overlay starts dismissed",
                              now > beat ? now - beat : 0ull, g_phase.load(), g_limit.load() / 1000, kExitDismissed);
    return 0;
}

// Ends the process if preparing a screen, a live screen's loop, or closing its videos stops
// moving (watchdog.h). The launcher then starts a parked overlay, dismissed, and Windows' own
// screen is back within seconds. A long gap between its own rounds is the machine having slept:
// the count starts again then, rather than ending an overlay that was only asleep.
void StartWatchdog() {
    std::thread([] {
        watchdog::Rule rule;
        for (;;) {
            Sleep((DWORD)watchdog::kRoundMs);
            const bool watched = g_watched.load();
            ULONGLONG gap = 0;
            const watchdog::Verdict v = rule.Round(GetTickCount64(), g_heartbeat.load(), g_limit.load(), &gap);
            if (v == watchdog::Verdict::AfterGap && watched) {
                animelogon::log::TryWrite(L"watchdog: %llu ms passed between two of its rounds (the machine slept) -- "
                                          L"the count starts again", gap);
            } else if (v == watchdog::Verdict::Stuck && watched && g_watched.load()) {
                if (HANDLE said = CreateThread(nullptr, 0, SayStuck, nullptr, 0, nullptr)) {
                    WaitForSingleObject(said, 500);
                    CloseHandle(said);
                }
                TerminateProcess(GetCurrentProcess(), kExitDismissed);
            }
        }
    }).detach();
}

// A crash ends the process at once. Windows Error Reporting would otherwise hold it -- and its
// topmost windows, over the password box -- while it collected a report. The exception's code is
// the exit code, so the launcher's log says what happened; the launcher counts any such ending
// as dismissed (overlayexit.h).
LONG WINAPI EndAtOnce(EXCEPTION_POINTERS *e) {
    const DWORD code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : overlayexit::kDismissed;
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The C runtime's own way out for a bad argument goes straight to error reporting.
void BadArgument(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {
    constexpr DWORD kInvalidCRuntimeParameter = 0xC0000417;
    TerminateProcess(GetCurrentProcess(), kInvalidCRuntimeParameter);
}

void EndOnCrash() {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(EndAtOnce);
    _set_invalid_parameter_handler(BadArgument);
    // abort() -- std::terminate's end too -- exits with code 3 without asking for a report.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    EndOnCrash();
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
    if (!opt.stillsPreview.empty()) {
        DeclareDpi();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        const bool ok = OtherDisplays::Preview(opt.stillsPreview);
        MFShutdown();
        return ok ? 0 : 1;
    }
    animelogon::log::Open(animelogon::paths::LogPath(L"overlay.log"));
    DeclareDpi();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT mfStart = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfStart)) ALOG(L"overlay: MFStartup failed (0x%08X)", mfStart);
    ProcessIdToSessionId(GetCurrentProcessId(), &g_overlay.session);
    g_overlay.hadUser = opt.hadUser;

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
    Scenes scenes;
    if (!presenter.CreateDevice()) return (int)overlayexit::kNoDevice;
    if (!opt.windowed) LoadStill(presenter);
    g_overlay.seenAt = GetTickCount64();
    StartWatchdog();

    // Park, appear, hand back, park again. Once dismissed, the parked wait holds the video off
    // the screen until the session is used and locked afresh.
    bool dismissed = opt.dismissed;
    for (;;) {
        if (!opt.windowed && !opt.now && !WaitForLogonScreen(presenter, scenes, &dismissed)) break;

        LiveEnd end = GoLiveOnce(presenter, scenes, opt, wc.hInstance);
        while (end == LiveEnd::Rebuild) {
            Sleep(300);  // let the mode change settle
            // A display change can come in the middle of a shutdown (drivers unloading), so the
            // screen is covered again only if the gate would cover it now.
            if (!opt.windowed && !opt.now && !LogonScreenNow()) {
                ALOG(L"overlay: the displays changed, and this is no longer a screen to cover -- staying off");
                end = LiveEnd::Dismissed;
                break;
            }
            end = GoLiveOnce(presenter, scenes, opt, wc.hInstance);
        }

        if (opt.windowed || opt.now) break;  // development runs are one-shot
        if (end == LiveEnd::Dismissed) {
            if (presenter.DeviceLost()) {
                presenter.ReleaseDevice();
                if (!presenter.CreateDevice()) break;
                LoadStill(presenter);
            }
            dismissed = true;
            continue;
        }
        if (end == LiveEnd::NoDisplays) continue;  // parked, not dismissed: covered once a display is back
        if (end == LiveEnd::LogonGone) {
            if (presenter.DeviceLost()) {
                presenter.ReleaseDevice();
                if (!presenter.CreateDevice()) break;
                LoadStill(presenter);
            }
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
