// overlay.exe: plays the imported video on the Winlogon desktop until a key or click, then
// fades out and hands the screen back to Windows. Started as SYSTEM by the service, one per
// logon screen. It parks between appearances: once dismissed it stays out of the way until the
// session is used and locked again, so it never fights somebody signing in. `--windowed` runs
// it on the ordinary desktop, for looking at a change without a lock screen.

#include <windows.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shellscalingapi.h>

#include <wtsapi32.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "animelogon/instance.h"
#include "animelogon/library.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/monitors.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"

#include "audio.h"
#include "bake.h"
#include "clockface.h"
#include "clocktext.h"
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

// The parked wait: return when a logon screen is the desktop on the glass, or the service is
// stopping. `dismissed` is true after somebody has waved the video away; while it is, the wait
// holds -- without re-covering the screen -- until the session is actually used (signed in and
// unlocked, or the secure desktop is gone), so the video never fights somebody signing in. It is
// cleared there, and the next genuine lock brings the video back. Returns false to exit.
bool WaitForLogonScreen(bool *dismissed) {
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
        if (screen::Decide(in) == screen::Gate::GoLive) return true;
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

// One appearance: resolve the settings, open the videos, cover the screen and play until
// somebody arrives or the screen is handed back. Returns why it ended.
LiveEnd GoLiveOnce(Presenter &presenter, const Options &opt, HINSTANCE instance) {
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
    // check; every video file it names is trust-checked below before the SYSTEM decoder
    // touches it.
    std::wstring why;
    animelogon::Settings settings = animelogon::LoadSettings(false, &why);
    if (!why.empty()) ALOG(L"settings: %s -- using defaults", why.c_str());

    std::vector<animelogon::MonitorInfo> monitors = animelogon::EnumerateMonitors();
    std::vector<Presenter::Target> targets;
    if (opt.windowed) {
        Presenter::Target t;
        const animelogon::MonitorInfo &m = monitors.front();
        t.rect = {m.rect.left + 80, m.rect.top + 80, m.rect.left + 80 + 960, m.rect.top + 80 + 540};
        t.canvas = t.rect;
        t.clock = settings.clock.enabled;
        t.videoId = settings.video.empty() ? (animelogon::ListVideos().empty() ? std::wstring()
                                                                               : animelogon::ListVideos().front().id)
                                           : settings.video;
        targets.push_back(t);
    } else {
        targets = plan::Build(settings, monitors);
    }

    // Open one decoder per distinct video, before any window is shown, so that if there is
    // nothing to play the screen is left to Windows rather than covered with black.
    std::map<std::wstring, std::shared_ptr<VideoPlayer>> players;
    for (const Presenter::Target &t : targets) {
        if (t.videoId.empty() || players.count(t.videoId)) continue;
        const std::wstring file = animelogon::VideoFilePath(t.videoId);
        std::wstring trust;
        // The SYSTEM decoder only touches administrators-only content.
        if (!opt.windowed && !animelogon::secure::IsTrusted(file, &trust)) {
            ALOG(L"overlay: %s is not trusted (%s) -- skipping it", t.videoId.c_str(), trust.c_str());
            continue;
        }
        auto player = std::make_shared<VideoPlayer>();
        if (player->Open(file, presenter.device(), presenter.videoManager()))
            players[t.videoId] = player;
        else
            ALOG(L"overlay: %s could not be opened", t.videoId.c_str());
    }

    if (presenter.DeviceLost()) {
        ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
        return LiveEnd::Leave;
    }
    // A display whose video cannot play is left to Windows, never covered with black.
    if (!opt.windowed) {
        std::vector<Presenter::Target> playable;
        for (const Presenter::Target &t : targets)
            if (players.count(t.videoId)) playable.push_back(t);
        targets.swap(playable);
    }
    if (targets.empty() || (!opt.windowed && players.empty())) {
        ALOG(L"overlay: no video to show -- leaving the screen to Windows");
        return LiveEnd::Dismissed;  // park until the screen is used
    }

    // The clock is drawn into the video's own frames, so it fades with them.
    ClockFace clock;
    bool anyClock = false;
    for (const Presenter::Target &t : targets) anyClock = anyClock || t.clock;
    const bool withClock = anyClock && clock.Init(presenter.device(), settings.clock, clocktext::Resolve(settings.clock));

    if (!presenter.CreateWindows(targets, kWindowClass, instance)) {
        ALOG(L"overlay: could not create the windows");
        return LiveEnd::Leave;
    }
    g_overlay.inputWindow = presenter.primary();
    if (withClock) presenter.SetClock(&clock);
    // By default the clock goes where Windows puts its password box, which LogonUI may not
    // have drawn yet; it is looked for again below until found.
    bool clockPlaced = !withClock || opt.windowed || settings.clock.displays != animelogon::ClockDisplays::Auto;
    auto placeClock = [&] {
        RECT monitor{};
        if (!clockPlaced && screen::CredentialMonitor(&monitor) && presenter.MoveClockTo(monitor)) clockPlaced = true;
    };
    placeClock();

    // Without raw input nothing could wake the screen, so it is never covered.
    if (!inputsink::Register(g_overlay.inputWindow)) {
        ALOG(L"overlay: raw input could not be registered (%lu) -- leaving the screen to Windows", GetLastError());
        presenter.DestroyWindows();
        g_overlay.inputWindow = nullptr;
        return LiveEnd::Dismissed;
    }

    AudioSystem audio;
    audio.Open(settings.audio);
    const double refresh = RefreshSeconds();
    if (!opt.windowed) {
        AudioSystem::RememberConsoleDefault();
        const std::wstring &primaryVideo = targets.front().videoId;
        if (!primaryVideo.empty()) {
            animelogon::VideoInfo info;
            if (animelogon::LoadVideo(primaryVideo, &info) && info.hasAudio)
                audio.PlayVideoTrack(animelogon::AudioFilePath(primaryVideo), 0.0, kFadeSeconds);
        }
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
    // Hidden until every display has its first frame, so the change from Windows' background
    // (that frame, baked) to the video is not a change at all.
    for (const ULONGLONG until = GetTickCount64() + kFirstFrameWaitMs;;) {
        Beat();
        std::vector<Presenter::Picture> pictures(targets.size());
        std::vector<VideoPlayer::Frame> held(targets.size());
        bool all = true;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto it = players.find(targets[i].videoId);
            bool changed = false;
            if (it != players.end() && it->second->FrameAt(0.0, &held[i], &changed)) {
                pictures[i].frame = &held[i];
                pictures[i].videoW = it->second->width();
                pictures[i].videoH = it->second->height();
            } else {
                all = false;
            }
        }
        if (all || GetTickCount64() >= until) {
            presenter.Render(pictures, settings.scaling, 0.0f, 1.0f);
            break;
        }
        Sleep(5);
    }
    Beat();
    presenter.Show();
    g_live.store(true);
    ALOG(L"overlay: live on %zu display(s)", targets.size());

    // --- frame loop --------------------------------------------------------------------
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const ULONGLONG start = GetTickCount64();
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
    bool done = false;
    LiveEnd end = LiveEnd::Leave;

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
        const bool clockChanged = withClock && clock.Tick();

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

        if (draw) {
            std::vector<Presenter::Picture> pictures(targets.size());
            std::vector<VideoPlayer::Frame> held(targets.size());
            // Drawn only when something on it changed: a high refresh rate need not cost a
            // full-screen draw per tick.
            bool fresh = lit || clockChanged || g_overlay.woke;
            for (size_t i = 0; i < targets.size(); ++i) {
                auto it = players.find(targets[i].videoId);
                if (it == players.end()) continue;
                bool changed = false;
                if (it->second->FrameAt(t, &held[i], &changed)) {
                    fresh = fresh || changed;
                    if (i == 0 && changed) {
                        ++shownFrames;
                        if (firstT < 0) firstT = t;
                        lastT = t;
                    }
                    pictures[i].frame = &held[i];
                    pictures[i].videoW = it->second->width();
                    pictures[i].videoH = it->second->height();
                }
            }
            if (fresh && !presenter.Render(pictures, settings.scaling, 0.0f, opacity)) {
                if (presenter.DeviceLost()) {
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

        for (auto &[id, player] : players)
            if (!done && player->failed()) {
                done = true;
                if (presenter.DeviceLost()) {
                    ALOG(L"overlay: the graphics device was lost -- exiting so a fresh one starts");
                    end = LiveEnd::Leave;
                } else {
                    ALOG(L"overlay: %s stopped decoding -- leaving the screen", id.c_str());
                    end = LiveEnd::Dismissed;
                }
            }

        // Leave when the logon screen does (real mode), after three readings.
        if (!opt.windowed && now - lastExitCheck > 500) {
            lastExitCheck = now;
            placeClock();
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
        const auto it = players.find(targets.front().videoId);
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
    StartWatchdog();

    // Park, appear, hand back, park again. Once dismissed, the parked wait holds the video off
    // the screen until the session is used and locked afresh.
    bool dismissed = opt.dismissed;
    for (;;) {
        if (!opt.windowed && !opt.now && !WaitForLogonScreen(&dismissed)) break;

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
