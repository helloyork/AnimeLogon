// The lock screen on several displays: noticing that the displays changed, which display holds
// the password box, the wall's blur when spanning, the wall leaving as one when woken, which
// displays get a window, and waiting out a moment with no display at all.
#include "check.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

#include "animelogon/blur.h"
#include "animelogon/components.h"
#include "animelogon/monitors.h"
#include "animelogon/settings.h"
#include "displaylayout.h"
#include "layout.h"
#include "plan.h"
#include "screen.h"
#include "wall.h"

using namespace animelogon;

namespace {

MonitorInfo Display(const wchar_t *key, RECT rect, bool primary) {
    MonitorInfo m;
    m.key = key;
    m.rect = rect;
    m.primary = primary;
    return m;
}

// The built-in themes only: nothing is read from the disk.
class BuiltIns : public ThemeStore {
public:
    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        return ::animelogon::LoadTheme(id, t, why);
    }
    bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) const override {
        return ::animelogon::LoadWallpaper(id, info, why);
    }
    bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) const override {
        return ::animelogon::LoadComponent(id, component, why);
    }
};

}  // namespace

// The layout the windows were made for against the displays now: order does not depend on how
// Windows lists them, and any display that moved, changed size, came or went is a change.
TEST(DisplayLayoutNoticesEveryChange) {
    const RECT main{0, 0, 1920, 1080}, side{1920, 0, 3200, 720}, left{-1280, 0, 0, 720};
    const std::vector<RECT> two = displaylayout::Ordered({{side, false}, {main, true}});
    CHECK(two.size() == 2 && EqualRect(&two[0], &main) && EqualRect(&two[1], &side));
    CHECK(displaylayout::Same(two, displaylayout::Ordered({{main, true}, {side, false}})));
    // The side display moved to the left (L2), came off (one display), or changed resolution.
    CHECK(!displaylayout::Same(two, displaylayout::Ordered({{main, true}, {left, false}})));
    CHECK(!displaylayout::Same(two, displaylayout::Ordered({{main, true}})));
    CHECK(!displaylayout::Same(two, displaylayout::Ordered({{main, true}, {{1920, 0, 4480, 1440}, false}})));
    // The other display became the primary.
    CHECK(!displaylayout::Same(two, displaylayout::Ordered({{main, false}, {side, true}})));
    // No display at all is a layout too, and differs from any other.
    CHECK(displaylayout::Ordered({}).empty() && !displaylayout::Same(two, {}));
    // Whether a still's display is still there.
    CHECK(displaylayout::Holds(two, side) && !displaylayout::Holds(displaylayout::Ordered({{main, true}}), side));
}

// LogonUI's largest window decides only among windows within one display; a window spanning the
// wall would put the password box on the biggest display. All of them spanning: the main display.
TEST(PasswordBoxDisplayIgnoresWindowsSpanningDisplays) {
    const std::vector<RECT> displays{{0, 0, 1920, 1080}, {1920, 0, 4480, 1440}};  // the side one bigger (L5)
    const RECT wall{0, 0, 4480, 1440}, box{0, 0, 1920, 1080}, small{700, 400, 1220, 700};
    screen::CredentialChoice c = screen::ChooseCredentialDisplay({wall, box}, displays, 0);
    CHECK(c.display == 0 && std::wstring(c.why).find(L"within one display") != std::wstring::npos);
    // Only the spanning window: the main display, wherever it is listed.
    c = screen::ChooseCredentialDisplay({wall}, displays, 0);
    CHECK(c.display == 0 && std::wstring(c.why).find(L"span") != std::wstring::npos);
    c = screen::ChooseCredentialDisplay({wall}, {displays[1], displays[0]}, 1);
    CHECK(c.display == 1);
    // Windows put the box on the side display: its window there wins over a smaller one on the main.
    c = screen::ChooseCredentialDisplay({small, {1920, 0, 4480, 1440}}, displays, 0);
    CHECK(c.display == 1);
    // Nothing of LogonUI's yet: not found, so the components stay where they started.
    CHECK(screen::ChooseCredentialDisplay({}, displays, 0).display == -1);
    CHECK(screen::ChooseCredentialDisplay({box}, {}, -1).display == -1);
}

// Spanning, every display's part of the wall is blurred by the primary display's height, exactly as
// the settings app's pictures of the wall are (app::WallSigma); otherwise each by its own.
TEST(SpanBlurIsMeasuredByThePrimaryDisplay) {
    const std::vector<MonitorInfo> monitors{Display(L"00000000000000aa", {0, 0, 1920, 1080}, true),
                                            Display(L"00000000000000bb", {0, 1080, 1920, 3240}, false)};  // L3, taller
    Settings s;
    s.theme = kDefaultTheme;
    s.themeOverrides[kDefaultTheme].blur = 2.8f;
    s.monitorMode = MonitorMode::Span;
    const plan::Plan span = plan::Build(s, monitors, BuiltIns());
    CHECK(span.displays.size() == 2);
    for (const plan::Display &d : span.displays) {
        const int h = d.rect.bottom - d.rect.top;
        CHECK(d.wallpaper.blur == 2.8f && d.wallpaper.blurHeight == 1080);
        CHECK(std::fabs(plan::BlurSigma(d.wallpaper, h) - blur::Sigma(2.8f, 1080)) < 1e-4f);
    }
    // The same wall in the settings app, at full size: the same sigma.
    const std::vector<app::Screen> wall = app::Wall(s, monitors, 1920, 3240);
    CHECK(std::fabs(app::WallSigma(2.8f, wall, 3240) - plan::BlurSigma(span.displays[1].wallpaper, 2160)) < 1e-3f);
    // The built-in wallpaper standing in for one that cannot be shown keeps the measure.
    std::vector<plan::Wallpaper> shown{span.displays[1].wallpaper};
    shown[0].source = plan::Source::Video;
    shown[0].id = L"1111111111111111";
    CHECK(plan::ShowBuiltInInstead(&shown, L"1111111111111111").size() == 1 && shown[0].blurHeight == 1080);
    // Duplicated: each display by its own height.
    s.monitorMode = MonitorMode::Duplicate;
    const plan::Plan dup = plan::Build(s, monitors, BuiltIns());
    CHECK(dup.displays[1].wallpaper.blurHeight == 0);
    CHECK(std::fabs(plan::BlurSigma(dup.displays[1].wallpaper, 2160) - blur::Sigma(2.8f, 2160)) < 1e-4f);
}

// Woken, a spanning wall grows about its own middle and every display moves up the same distance;
// a display of its own grows about its centre and moves up by its own height.
TEST(WakeTransitionMovesTheWallAsOne) {
    const RECT main{0, 0, 1920, 1080}, side{1920, 0, 3200, 720};
    const RECT wall{0, 0, 3200, 1080};
    const layout::Leave a = layout::LeaveOf(main, wall, 1.08f, -0.5f), b = layout::LeaveOf(side, wall, 1.08f, -0.5f);
    // One point in desktop pixels: the wall's middle, (1600, 540).
    CHECK(a.centreX + main.left == 1600.0f && b.centreX + side.left == 1600.0f);
    CHECK(a.centreY + main.top == 540.0f && b.centreY + side.top == 540.0f);
    CHECK(a.dy == -540.0f && b.dy == -540.0f && a.scale == 1.08f);
    // Gone at the end: the whole wall's height up, which clears every display of it.
    CHECK(layout::LeaveOf(side, wall, 1.0f, -1.0f).dy == -1080.0f);
    // On its own (duplicated, per display): about its own centre, by its own height.
    const layout::Leave own = layout::LeaveOf(side, side, 1.08f, -1.0f);
    CHECK(own.centreX == 640.0f && own.centreY == 360.0f && own.dy == -720.0f);
    CHECK(layout::LeaveOf(side, side, 1.0f, 0.0f).dy == 0.0f);
}

// A display without an area gets no window, and the windows after it still show their own
// display's picture: each keeps its display's index.
TEST(WindowsKeepTheirOwnDisplay) {
    const std::vector<size_t> all = layout::WithArea({{0, 0, 1920, 1080}, {1920, 0, 3200, 720}});
    CHECK(all.size() == 2 && all[0] == 0 && all[1] == 1);
    const std::vector<size_t> skipped = layout::WithArea({{0, 0, 1920, 1080}, {1920, 0, 1920, 720}, {3200, 0, 4480, 720}});
    CHECK(skipped.size() == 2 && skipped[0] == 0 && skipped[1] == 2);
    CHECK(layout::WithArea({}).empty());
}

// A logon screen to cover but no display attached -- a DisplayPort monitor powered down -- is
// waited out parked, not taken for the screen having been waved away.
TEST(GateWaitsForADisplay) {
    screen::GateInput in;
    in.secure = true;
    in.session.console = true;
    in.noDisplays = true;
    CHECK(screen::Decide(in) == screen::Gate::NoDisplays);
    in.noDisplays = false;
    CHECK(screen::Decide(in) == screen::Gate::GoLive);
    // Never a reason to cover what the gate otherwise keeps off.
    in.noDisplays = true;
    in.goingDown = true;
    CHECK(screen::Decide(in) == screen::Gate::GoingDown);
    in.goingDown = false;
    in.secure = false;
    CHECK(screen::Decide(in) == screen::Gate::Hold);
}
