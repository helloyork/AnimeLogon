// The pictures the displays other than the primary show from a wake until sign-in
// (otherdisplays.h): which displays get one, what each is cut from, and how it is made. No
// windows and no threads here, so it is tested directly.
//
// Windows draws nothing on those displays while it asks for the password -- they stay black
// from the lock until sign-in -- so each keeps a still of its own wallpaper instead: the
// picture it was showing, without the components, blurred and darkened the way Windows shows
// the sign-in background behind the password box on the primary.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "plan.h"

namespace stills {

// One display's still.
struct Still {
    std::wstring monitorKey;
    RECT rect{};    // the display, in desktop pixels
    RECT canvas{};  // what its wallpaper is fitted to: the display, or the wall when spanning
    plan::Wallpaper wallpaper;
};

// The displays that get a still, in the plan's order: every display but the primary -- where
// Windows draws the password box -- that shows a wallpaper. Each is cut from what that display
// shows: its own theme's wallpaper on its own canvas, which is its slice of the wall when
// spanning. None when `mode` is Black or there is one display.
std::vector<Still> Choose(const plan::Plan &plan, animelogon::OtherDisplays mode);

// The display's picture of a wallpaper that is an image -- an image wallpaper, a scene's still,
// the built-in one -- laid out as the overlay draws it: B G R, 3 bytes a pixel, rows top to
// bottom, the display's size. False, with `why`, for a video (bake::FirstFrameOn makes those)
// and for an image that cannot be read.
bool Compose(const Still &still, std::vector<uint8_t> *bgr, std::wstring *why);

// The built-in wallpaper on the still's display, for a wallpaper that cannot be shown: what the
// overlay itself puts in its place.
std::vector<uint8_t> BuiltIn(const Still &still);

// The still as its window shows it, from Compose's picture: B G R A, every pixel opaque, into
// `bgra` (width * height * 4 bytes). Blur softens and darkens it (Soften); Picture keeps it as
// it is.
void Finish(const std::vector<uint8_t> &bgr, int width, int height, animelogon::OtherDisplays mode, uint8_t *bgra);

// The look of Windows' own sign-in background behind the password box, measured on a test card
// (Windows 11 26200, 1920x1080): a Gaussian blur of about 30 pixels -- kBlurSigma of the
// display's height -- then each pixel pulled toward dark grey, Tone, and a fine grain of about
// one level.
constexpr float kBlurSigma = 0.028f;
constexpr float kLevel = 0.655f;      // white comes out at 180, black at kLift
constexpr float kLift = 13.5f;
constexpr float kSaturation = 1.15f;  // colours keep a little more of their colour than grey does
constexpr float kGrain = 2.5f;        // the grain's reach, in levels either way

// One blurred pixel's B G R, 0..255, as it is shown.
void Tone(const float bgr[3], float out[3]);

// The blurred, toned picture, from and into B G R A at width x height (alpha is set to 255).
// Done on a grid a few times coarser than the display, where the blur is cheap, and brought back
// up smoothly; the grain also keeps wide gradients from banding.
void Soften(int width, int height, uint8_t *bgra);

}  // namespace stills
