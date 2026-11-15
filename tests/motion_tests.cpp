// How the logon screen moves: each component panel's way in (enter, enter-delay, enter-duration)
// and a theme's wake transition (<wake>, theme.<id>.wake and .wake-duration).
#include "check.h"

#include <string>

#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "animelogon/theme.h"

using namespace animelogon;

namespace {

const std::string kWallpaper = "<wallpaper ref=\"default\" fit=\"fill\"/>";

std::string Theme(const std::string &body) { return "<theme format=\"1\" name=\"t\">" + kWallpaper + body + "</theme>"; }

bool ParsesTheme(const std::string &text, theme::Theme *out = nullptr) {
    theme::Theme t;
    const bool ok = theme::Parse(text, theme::Form::Installed, &t, nullptr);
    if (out) *out = t;
    return ok;
}

}  // namespace

TEST(PanelsComeInAsTheirComponentSays) {
    // The built-in clock says nothing: a fade over 0.4 s, at once.
    const skin::Panel &plain = skin::Resolve(skin::Default(), {}).panels[0];
    CHECK(plain.enter == skin::Enter::Fade && plain.enterDelay == 0.0f && plain.enterDuration == 0.4f);
    // Adjusted by hand, within range; anything else is ignored.
    const skin::Panel &p = skin::Resolve(skin::Default(), {{L"p1.enter", L"rise"},
                                                           {L"p1.enter-delay", L"1.5"},
                                                           {L"p1.enter-duration", L"0.8"}})
                               .panels[0];
    CHECK(p.enter == skin::Enter::Rise && p.enterDelay == 1.5f && p.enterDuration == 0.8f);
    const skin::Panel &bad = skin::Resolve(skin::Default(), {{L"p1.enter", L"spin"},
                                                             {L"p1.enter-delay", L"11"},
                                                             {L"p1.enter-duration", L"-1"}})
                                 .panels[0];
    CHECK(bad.enter == skin::Enter::Fade && bad.enterDelay == 0.0f && bad.enterDuration == 0.4f);
    CHECK(skin::IsAdjustmentKey(L"p2.enter") && skin::IsAdjustmentValue(L"p1.enter", L"zoom") &&
          skin::IsAdjustmentValue(L"p1.enter", L"none") && !skin::IsAdjustmentValue(L"p1.enter", L"slide"));
    // The settings app offers all three on every panel.
    const std::vector<skin::Part> parts = skin::Parts(skin::Default());
    int offered = 0;
    for (const skin::Adjustment &a : parts[0].adjustments)
        offered += a.key == L"p1.enter" || a.key == L"p1.enter-delay" || a.key == L"p1.enter-duration";
    CHECK(offered == 3);
    // A component may say it itself.
    skin::Skin own;
    CHECK(skin::Parse("<component format=\"1\" name=\"c\"><panel enter=\"zoom\" enter-delay=\"2\">"
                      "<line><text value=\"{time}\"/></line></panel></component>",
                      &own, nullptr));
    const skin::Panel &z = skin::Resolve(own, {}).panels[0];
    CHECK(z.enter == skin::Enter::Zoom && z.enterDelay == 2.0f);
    CHECK(!skin::Parse("<component format=\"1\" name=\"c\"><panel enter=\"spin\">"
                       "<line><text value=\"{time}\"/></line></panel></component>",
                       &own, nullptr));
}

TEST(ThemesSayHowTheyGiveWay) {
    theme::Theme t;
    // Nothing said: a fade over half a second, and nothing written back.
    CHECK(ParsesTheme(Theme(""), &t) && t.wake == WakeStyle::Fade && t.wakeSeconds == kWakeSeconds);
    CHECK(theme::Normalize(t).find("<wake") == std::string::npos);
    CHECK(ParsesTheme(Theme("<wake style=\"slide\" duration=\"0.8\"/>"), &t));
    CHECK(t.wake == WakeStyle::Slide && t.wakeSeconds == 0.8f);
    theme::Theme again;
    CHECK(ParsesTheme(theme::Normalize(t), &again) && again.wake == WakeStyle::Slide && again.wakeSeconds == 0.8f);
    CHECK(ParsesTheme(Theme("<wake style=\"cut\"/>"), &t) && t.wake == WakeStyle::Cut && t.wakeSeconds == kWakeSeconds);
    CHECK(ParsesTheme(Theme("<wake duration=\"2\"/>"), &t) && t.wake == WakeStyle::Fade && t.wakeSeconds == 2.0f);
    // The shortest the settings app offers. As a double it is a hair below the float bound.
    CHECK(ParsesTheme(Theme("<wake duration=\"0.1\"/>"), &t) && t.wakeSeconds == kWakeSecondsMin);
    for (const char *bad : {"<wake style=\"spin\"/>", "<wake duration=\"0.05\"/>", "<wake duration=\"3\"/>",
                            "<wake duration=\"fast\"/>", "<wake speed=\"1\"/>", "<wake/><wake/>",
                            "<wake><set key=\"a\" value=\"b\"/></wake>"})
        CHECK(!ParsesTheme(Theme(bad)));

    // settings.ini changes it per theme, and says it back.
    const Settings s = ParseSettings(L"theme = default\r\ntheme.default.wake = zoom\r\ntheme.default.wake-duration = 1.25\r\n"
                                     L"theme.0123456789abcdef.wake = sideways\r\ntheme.0123456789abcdef.wake-duration = 9\r\n");
    const ThemeOverrides &o = s.OverridesFor(L"default");
    CHECK(o.wake == WakeStyle::Zoom && o.wakeSeconds == 1.25f);
    CHECK(!s.OverridesFor(L"0123456789abcdef").wake && !s.OverridesFor(L"0123456789abcdef").wakeSeconds);
    const Settings back = ParseSettings(SerializeSettings(s));
    CHECK(back.OverridesFor(L"default").wake == WakeStyle::Zoom && back.OverridesFor(L"default").wakeSeconds == 1.25f);
    const theme::Theme e = theme::Edited(theme::Default(), o);
    CHECK(e.wake == WakeStyle::Zoom && e.wakeSeconds == 1.25f);

    // 0.1 s, as the settings app writes it, is kept rather than turned away for the theme's own.
    std::vector<std::wstring> problems;
    const Settings shortest = ParseSettings(L"theme = default\r\ntheme.default.wake-duration = 0.1\r\ntheme.default.brightness = 0.1\r\n", &problems);
    CHECK(problems.empty());
    CHECK(shortest.OverridesFor(L"default").wakeSeconds == kWakeSecondsMin);
    CHECK(shortest.OverridesFor(L"default").brightness == kBrightnessMin);
    CHECK(!ParseSettings(L"theme.default.wake-duration = 0.09\r\n").OverridesFor(L"default").wakeSeconds);
}
