// How bright a theme's wallpaper is drawn: <wallpaper brightness>, theme.<id>.brightness, and the
// darkening every picture of the wallpaper gets (the logon screen's shader, the baked sign-in
// background, the other displays' stills, the settings app's pictures).
#include "check.h"

#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "animelogon/theme.h"
#include "plan.h"

using namespace animelogon;

namespace {

std::string Theme(const std::string &wallpaper) {
    return "<theme format=\"1\" name=\"t\">" + wallpaper + "</theme>";
}

bool ParsesTheme(const std::string &text, theme::Theme *out = nullptr) {
    theme::Theme t;
    const bool ok = theme::Parse(text, theme::Form::Installed, &t, nullptr);
    if (out) *out = t;
    return ok;
}

}  // namespace

TEST(ThemesSayHowBrightTheirWallpaperIs) {
    theme::Theme t;
    // Nothing said: as it is, and nothing written back, so a theme that never set it stays as it was.
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\"/>"), &t) && t.brightness == kBrightness);
    CHECK(theme::Normalize(t).find("brightness") == std::string::npos);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\" brightness=\"0.7\"/>"), &t) && t.brightness == 0.7f);
    theme::Theme again;
    CHECK(theme::Normalize(t).find("brightness=\"0.7\"") != std::string::npos);
    CHECK(ParsesTheme(theme::Normalize(t), &again) && again.brightness == 0.7f);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fit\" brightness=\"0.1\"/>"), &t) && t.brightness == 0.1f);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fit\" brightness=\"1\"/>"), &t) && t.brightness == 1.0f);
    for (const char *bad : {"0", "0.05", "1.2", "-0.5", "dim", "", "0.7 "})
        CHECK(!ParsesTheme(Theme(std::string("<wallpaper ref=\"default\" fit=\"fill\" brightness=\"") + bad + "\"/>")));

    // settings.ini changes it per theme, and says it back; a bad value is not taken.
    const Settings s = ParseSettings(L"theme = default\r\ntheme.default.brightness = 0.55\r\n"
                                     L"theme.0123456789abcdef.brightness = 2\r\n");
    CHECK(s.OverridesFor(L"default").brightness == 0.55f);
    CHECK(!s.OverridesFor(L"0123456789abcdef").brightness);
    const Settings back = ParseSettings(SerializeSettings(s));
    CHECK(back.OverridesFor(L"default").brightness == 0.55f);
    CHECK(SerializeSettings(s).find(L"theme.default.brightness = 0.55") != std::wstring::npos);
    CHECK(theme::Edited(theme::Default(), s.OverridesFor(L"default")).brightness == 0.55f);
    CHECK(theme::Edited(theme::Default(), {}).brightness == kBrightness);
}

TEST(BrightnessScalesEveryColourAlike) {
    // B G R: each channel scaled and rounded to nearest; black stays black.
    std::vector<uint8_t> bgr = {255, 128, 0, 10, 200, 3};
    ApplyBrightness(bgr.data(), 2, 3, 0.5f);
    CHECK((bgr == std::vector<uint8_t>{128, 64, 0, 5, 100, 2}));
    // B G R A: the fourth byte is left alone.
    std::vector<uint8_t> bgra = {200, 100, 50, 255, 0, 0, 0, 17};
    ApplyBrightness(bgra.data(), 2, 4, 0.25f);
    CHECK((bgra == std::vector<uint8_t>{50, 25, 13, 255, 0, 0, 0, 17}));
    // At full brightness nothing changes, not even by rounding.
    std::vector<uint8_t> same = {1, 2, 3, 254};
    ApplyBrightness(same.data(), 1, 4, 1.0f);
    CHECK((same == std::vector<uint8_t>{1, 2, 3, 254}));
}

TEST(BrightnessFollowsTheWallpaperOntoTheScreen) {
    // A wallpaper that cannot be shown gives way to the built-in one at the same brightness: the
    // theme's look stays, whatever stands in for its picture.
    plan::Wallpaper video;
    video.source = plan::Source::Video;
    video.id = L"1111111111111111";
    video.brightness = 0.6f;
    std::vector<plan::Wallpaper> shown = {video};
    CHECK(plan::ShowBuiltInInstead(&shown, L"1111111111111111").size() == 1);
    CHECK(shown[0].source == plan::Source::Gradient && shown[0].brightness == 0.6f);
}
