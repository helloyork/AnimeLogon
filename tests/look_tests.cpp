// How bright a theme's wallpaper is drawn: <wallpaper brightness>, theme.<id>.brightness, and the
// darkening every picture of the wallpaper gets (the logon screen's shader, the baked sign-in
// background, the other displays' stills, the settings app's pictures).
#include "check.h"

#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/blur.h"
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

TEST(ThemesSayHowMuchTheirWallpaperIsBlurred) {
    theme::Theme t;
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\"/>"), &t) && t.blur == 0.0f);
    CHECK(theme::Normalize(t).find("blur") == std::string::npos);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\" brightness=\"0.8\" blur=\"2.8\"/>"), &t));
    CHECK(t.blur == 2.8f && t.brightness == 0.8f);
    theme::Theme again;
    CHECK(theme::Normalize(t).find("blur=\"2.8\"") != std::string::npos);
    CHECK(ParsesTheme(theme::Normalize(t), &again) && again.blur == 2.8f);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\" blur=\"0\"/>"), &t) && t.blur == 0.0f);
    CHECK(ParsesTheme(Theme("<wallpaper ref=\"default\" fit=\"fill\" blur=\"5\"/>"), &t) && t.blur == 5.0f);
    for (const char *bad : {"-1", "5.1", "soft", "", "1 "})
        CHECK(!ParsesTheme(Theme(std::string("<wallpaper ref=\"default\" fit=\"fill\" blur=\"") + bad + "\"/>")));

    const Settings s = ParseSettings(L"theme = default\r\ntheme.default.blur = 1.5\r\ntheme.0123456789abcdef.blur = 9\r\n");
    CHECK(s.OverridesFor(L"default").blur == 1.5f && !s.OverridesFor(L"0123456789abcdef").blur);
    CHECK(ParseSettings(SerializeSettings(s)).OverridesFor(L"default").blur == 1.5f);
    CHECK(theme::Edited(theme::Default(), s.OverridesFor(L"default")).blur == 1.5f);
}

TEST(BlurWorksOnCellsAndKeepsWhatIsEven) {
    // Cells: the largest power of two up to half the sigma.
    CHECK(blur::Factor(0.5f) == 1 && blur::Factor(3.9f) == 1 && blur::Factor(4.0f) == 2 && blur::Factor(30.0f) == 8);
    CHECK(blur::Factor(108.0f) == 32);
    CHECK(blur::Sigma(2.8f, 1080) > 30.2f && blur::Sigma(2.8f, 1080) < 30.3f);
    // An even picture stays as it is, edges included; the fourth byte is never touched.
    std::vector<uint8_t> even(40 * 30 * 4);
    for (size_t i = 0; i < even.size(); i += 4) even[i] = 10, even[i + 1] = 120, even[i + 2] = 250, even[i + 3] = 7;
    std::vector<uint8_t> blurred = even;
    blur::Apply(blurred.data(), 40, 30, 4, 9.0f);
    CHECK(blurred == even);
    // A white line on black spreads out evenly to both sides and keeps its total.
    std::vector<uint8_t> line(64 * 8 * 3, 0);
    for (int y = 0; y < 8; ++y)
        for (int c = 0; c < 3; ++c) line[((size_t)y * 64 + 32) * 3 + c] = 255;
    std::vector<uint8_t> soft = line;
    blur::Apply(soft.data(), 64, 8, 3, 3.0f);
    const auto at = [&](int x) { return (int)soft[((size_t)4 * 64 + x) * 3]; };
    CHECK(at(32) < 255 && at(32) > 0 && at(28) > 0 && at(36) > 0 && at(10) == 0);
    CHECK(std::abs(at(29) - at(35)) <= 2 && std::abs(at(30) - at(34)) <= 2);
    int sum = 0;
    for (int x = 0; x < 64; ++x) sum += at(x);
    CHECK(std::abs(sum - 255) <= 8);
    // No blur, no change.
    std::vector<uint8_t> same = line;
    blur::Apply(same.data(), 64, 8, 3, 0.0f);
    CHECK(same == line);
}
