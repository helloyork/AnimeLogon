// The displays other than the primary, from a wake until sign-in.
//
// Windows draws nothing on them while it asks for the password: the Winlogon desktop is black
// there from the lock until sign-in. So when the screen wakes, each of them keeps a still of what
// it was showing (stills.h): its own wallpaper, without the components, blurred and darkened as
// Windows shows the sign-in background behind the password box on the primary -- or as it is, or
// nothing, as `other_displays` says.
//
// - Made after the screen is covered, on a thread of its own: decoding, fitting and blurring
//   never run on the frame thread. The frame thread only takes a finished still into a hidden
//   window, one window a tick, and later shows it.
// - At the wake each still is shown directly under our own windows, so the wake transition that
//   uncovers Windows' password screen on the primary uncovers the still on the others, in step
//   with it whatever the transition is. A still that is not ready by then fades in with the
//   transition's own progress, or after it over the transition's length.
// - It stays while the password screen is up and goes when it goes: at sign-in, when our
//   desktop stops being the one on the glass, or when the next appearance covers the screen.
//   It is made again when the displays change, and taken down on a display Windows puts its
//   password box on, and when Windows begins to shut down.
// - Never on the primary: that is where the password box is. Each window is layered,
//   transparent to input, never activated and never in the taskbar, so keys and clicks reach
//   what they reached before; nothing about it is input, so nothing about it can wake the screen.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "plan.h"
#include "stills.h"

class OtherDisplays {
public:
    OtherDisplays() = default;
    ~OtherDisplays();
    OtherDisplays(const OtherDisplays &) = delete;
    OtherDisplays &operator=(const OtherDisplays &) = delete;

    // Once the screen is covered: takes down any still left from before and starts making this
    // appearance's.
    void Prepare(const plan::Plan &plan, animelogon::OtherDisplays mode);

    // Once a tick while the screen is up. `woken` once somebody has arrived; `progress` is how
    // far the wake transition has gone, 0 at the wake to 1 when it is over.
    void Live(bool woken, float progress);

    // Once a tick on the password screen a wake left behind. Late stills fade in over
    // `appearSeconds`. True while one is fading in: the caller should tick again soon.
    bool Parked(float appearSeconds);

    // Takes every still down: the password screen is over.
    void Clear();

    // For development: writes, for every display, the still it would get under the current
    // settings both ways -- displayN-picture.bmp and displayN-blur.bmp, the primary's included
    // for comparing with Windows' own -- and stills.txt, saying which displays get one.
    static bool Preview(const std::wstring &dir);

private:
    struct Job;
    struct Window {
        stills::Still still;
        HWND hwnd = nullptr;
        bool shown = false;
        int alpha = -1;          // what it was last given, 0..255
        bool follows = false;    // fading in with the wake transition's progress
        ULONGLONG appearAt = 0;  // fading in on its own since then
    };

    void Start(const plan::Plan &plan, animelogon::OtherDisplays mode);
    bool AdoptOne();
    void Show(Window &w, int alpha);
    void SetAlpha(Window &w, int alpha);
    void Check();
    void Report();

    std::shared_ptr<Job> job_;
    size_t adopted_ = 0;  // how many of job_'s stills are in windows
    std::vector<Window> windows_;
    animelogon::OtherDisplays mode_ = animelogon::OtherDisplays::Blur;
    std::vector<RECT> layout_;  // every display, primary first, as the stills were made for
    bool woken_ = false;
    bool reported_ = false;
    bool stopped_ = false;  // Windows is shutting down: nothing more until the next appearance
    ULONGLONG checkedAt_ = 0;
};
