// The lock screen giving way to Windows rather than blocking somebody signing in: the watchdog's
// rule (and sleep), what the launcher makes of an overlay's exit code, "Switch user", a pen or a
// touch waking the screen, and when a parked overlay plans the displays again.
#include "check.h"

#include <windows.h>

#include <vector>

#include "overlayexit.h"
#include "parkedplan.h"
#include "wake.h"
#include "watchdog.h"

using watchdog::Rule;
using watchdog::Verdict;

// A main thread that beats is never stuck; one that stops is, once its limit has passed -- and the
// preparation's limit is looser than the frame loop's.
TEST(WatchdogEndsAStuckMainThread) {
    Rule rule;
    ULONGLONG now = 100000, beat = now;
    for (int i = 0; i < 40; ++i, now += watchdog::kRoundMs) {
        beat = now;
        CHECK(rule.Round(now, beat, watchdog::kLiveMs) == Verdict::Fine);
    }
    // Stuck from `beat`: fine up to the limit, ended after it.
    const ULONGLONG stuckAt = beat;
    bool ended = false;
    for (; now <= stuckAt + watchdog::kLiveMs + 1000; now += watchdog::kRoundMs) {
        const Verdict v = rule.Round(now, stuckAt, watchdog::kLiveMs);
        if (now - stuckAt <= watchdog::kLiveMs) CHECK(v == Verdict::Fine);
        else ended = ended || v == Verdict::Stuck;
    }
    CHECK(ended);
    // Preparing, the same silence is still allowed.
    Rule prepare;
    CHECK(prepare.Round(200000, 200000, watchdog::kPrepareMs) == Verdict::Fine);
    for (ULONGLONG t = 200500; t <= 200000 + 10000; t += watchdog::kRoundMs)
        CHECK(prepare.Round(t, 200000, watchdog::kPrepareMs) == Verdict::Fine);
    CHECK(watchdog::kPrepareMs >= 10000 && watchdog::kPrepareMs <= 15000 && watchdog::kLiveMs == 5000);
}

// Asleep, every thread stops and the tick count runs on. The first round after the resume comes
// long after the one before: the count starts again, and only a main thread that then stays still
// for the whole limit is ended.
TEST(WatchdogSkipsTheRoundAfterASleep) {
    Rule rule;
    ULONGLONG now = 500000;
    CHECK(rule.Round(now, now, watchdog::kLiveMs) == Verdict::Fine);
    const ULONGLONG lastBeat = now;
    // Two hours asleep, then the watchdog runs before the main thread does.
    now += 2ull * 3600 * 1000;
    ULONGLONG gap = 0;
    CHECK(rule.Round(now, lastBeat, watchdog::kLiveMs, &gap) == Verdict::AfterGap);
    CHECK(gap == 2ull * 3600 * 1000);
    // The main thread is back and beating: fine.
    now += watchdog::kRoundMs;
    CHECK(rule.Round(now, now - 10, watchdog::kLiveMs) == Verdict::Fine);
    // A short gap -- a busy machine, two seconds or less -- is not a sleep.
    now += watchdog::kGapMs;
    CHECK(rule.Round(now, now, watchdog::kLiveMs) == Verdict::Fine);
    // After a sleep the old beat does not count against it: the limit is counted from the resume.
    Rule late;
    ULONGLONG t = 900000;
    CHECK(late.Round(t, t, watchdog::kLiveMs) == Verdict::Fine);
    t += 60000;  // asleep a minute
    CHECK(late.Round(t, 900000, watchdog::kLiveMs) == Verdict::AfterGap);
    const ULONGLONG resumed = t;
    for (t += watchdog::kRoundMs; t <= resumed + watchdog::kLiveMs; t += watchdog::kRoundMs)
        CHECK(late.Round(t, 900000, watchdog::kLiveMs) == Verdict::Fine);
    CHECK(late.Round(t, 900000, watchdog::kLiveMs) == Verdict::Stuck);  // genuinely stuck since
}

// The launcher: the endings an overlay chooses mean what they say; anything else -- a crash, the
// watchdog, abort(), a code nobody planned -- counts as dismissed. A device that never came up
// changes nothing.
TEST(LauncherCountsAbnormalExitsAsDismissed) {
    using overlayexit::Judge;
    using overlayexit::StartsDismissed;
    CHECK(Judge(0) == overlayexit::Verdict::NotDismissed);
    CHECK(Judge(2) == overlayexit::Verdict::Unchanged);
    CHECK(Judge(3) == overlayexit::Verdict::Dismissed);
    CHECK(!StartsDismissed(0, true) && !StartsDismissed(0, false));
    CHECK(StartsDismissed(2, true) && !StartsDismissed(2, false));
    CHECK(StartsDismissed(3, false));
    for (const DWORD crash : {0xC0000005u, 0xC0000409u, 0xC0000417u, 0xC00000FDu, 0x40010004u, 1u, 259u, 0xFFFFFFFFu}) {
        CHECK(Judge(crash) == overlayexit::Verdict::Abnormal);
        CHECK(StartsDismissed(crash, false));
    }
    CHECK(overlayexit::kDismissed == 3 && overlayexit::kNoDevice == 2 && overlayexit::kNotWoken == 0);
}

// "Switch user" from a screen somebody waved away: the overlay in the new session starts dismissed.
// From a screen still showing the video (fast user switching from the Start menu), or after a sign-out
// (the old session lost its user), the new session's logon screen shows the video.
TEST(SwitchUserKeepsAWavedAwayScreenAway) {
    using overlayexit::SwitchStartsDismissed;
    CHECK(SwitchStartsDismissed(true, true));
    CHECK(!SwitchStartsDismissed(false, true));
    CHECK(!SwitchStartsDismissed(true, false));
    CHECK(!SwitchStartsDismissed(false, false));
}

// A pen or a finger touching the screen wakes it as a click does; the mouse and the touchpad are
// left to raw input; nothing from before the screen came up counts, nor anything after a wake.
TEST(PenAndTouchPressesWake) {
    wake::Intent intent;
    CHECK(!intent.ObservePointer(true, 2000));  // not armed: the screen is not up
    intent.Prime({}, 1000);
    CHECK(!intent.ObservePointer(true, 1000));   // queued before the screen came up
    CHECK(!intent.ObservePointer(false, 1500));  // a mouse or touchpad pointer
    CHECK(!intent.requested());
    CHECK(intent.ObservePointer(true, 1500));
    CHECK(intent.requested());
    CHECK(!intent.ObservePointer(true, 1600));   // already woken
    // Nothing else wakes it: a pointer that only moves or hovers sends no WM_POINTERDOWN at all.
    wake::Intent other;
    other.Prime({}, 1000);
    RAWINPUT move{};
    move.header.dwType = RIM_TYPEMOUSE;
    move.header.dwSize = sizeof(move);
    move.data.mouse.lLastX = 40;
    CHECK(!other.ObserveRaw(move, 1500) && !other.requested());
}

// The parked overlay plans again only when settings.ini or the displays change, or once a minute
// while a scene wallpaper is planned.
TEST(ParkedOverlayPlansOnlyOnAChange) {
    parkedplan::Watch watch;
    parkedplan::Stamp file{true, 1000, 300};
    const std::vector<RECT> one{{0, 0, 1920, 1080}}, two{{0, 0, 1920, 1080}, {1920, 0, 3200, 720}};
    CHECK(watch.Due(file, one, 0));  // the first time
    watch.Planned(false);
    CHECK(!watch.Due(file, one, 2000));
    CHECK(!watch.Due(file, one, 10 * 60 * 1000));  // no scene: never on a timer
    parkedplan::Stamp rewritten = file;
    rewritten.written = 2000;
    CHECK(watch.Due(rewritten, one, 10 * 60 * 1000 + 2000));
    CHECK(!watch.Due(rewritten, one, 10 * 60 * 1000 + 4000));
    CHECK(watch.Due(rewritten, two, 10 * 60 * 1000 + 6000));  // a display came
    watch.Planned(true);                                      // and there is a scene
    CHECK(!watch.Due(rewritten, two, 10 * 60 * 1000 + 8000));
    CHECK(watch.Due(rewritten, two, 10 * 60 * 1000 + 6000 + parkedplan::Watch::kRetryMs));
    parkedplan::Stamp gone;
    CHECK(watch.Due(gone, two, 12 * 60 * 1000 + 6000 + parkedplan::Watch::kRetryMs));  // settings.ini deleted
}
