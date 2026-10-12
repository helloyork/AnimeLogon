// The settings app's pictures of themes (app/src/pictures.h, wall.h): the wall of displays when
// spanning, and the components alone as a layer over a video the page plays. Drawn by the logon
// screen's own renderer, on WARP where there is no graphics card.
#include "check.h"

#include <windows.h>
#include <objbase.h>

#include <map>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/monitors.h"
#include "animelogon/settings.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "library.h"
#include "pictures.h"
#include "wall.h"

using namespace animelogon;

namespace {

const std::wstring kTheme = L"cccccccccccccccc";

// One theme: the built-in wallpaper and the clock at the bottom left.
class OneThemeStore : public ThemeStore {
public:
    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        if (id == kDefaultTheme) return *t = theme::Default(), true;
        if (id != kTheme) return false;
        return theme::Parse("<theme format=\"1\" name=\"W\"><wallpaper ref=\"default\" fit=\"fill\"/>"
                            "<component id=\"clock\" ref=\"clock\"><set key=\"p1.anchor\" value=\"bottom-left\"/>"
                            "</component></theme>",
                            theme::Form::Installed, t, why);
    }
    bool LoadWallpaper(const std::wstring &, WallpaperInfo *, std::wstring *) const override { return false; }
    bool LoadComponent(const std::wstring &id, skin::Skin *s, std::wstring *) const override {
        if (id != kClockComponent) return false;
        *s = skin::Default();
        return true;
    }
};

class OneThemeStores : public app::Stores {
public:
    OneThemeStore store;
    const ThemeStore &Store() const override { return store; }
    std::vector<ThemeEntry> Themes() const override {
        return {{kDefaultTheme, L"默认", L"AnimeLogon", true}, {kTheme, L"W", L"", false}};
    }
    std::vector<WallpaperInfo> Wallpapers() const override { return {}; }
    std::vector<ComponentEntry> Components() const override { return {{kClockComponent, L"时钟", L"AnimeLogon", true}}; }
    Settings ReadSettings() const override { return ParseSettings(L"theme = " + kTheme + L"\r\n"); }
    bool WriteSettings(const std::wstring &) const override { return true; }
};

MonitorInfo Monitor(const wchar_t *key, RECT rect, bool primary) {
    MonitorInfo m;
    m.key = key;
    m.rect = rect;
    m.primary = primary;
    return m;
}

// A 1920 x 1080 main display, and a 1280 x 1024 one to its right, 56 pixels lower.
std::vector<MonitorInfo> TwoDisplays() {
    return {Monitor(L"main", {0, 0, 1920, 1080}, true), Monitor(L"side", {1920, 56, 3200, 1080}, false)};
}

bool Within(const D2D1_RECT_F &r, const RECT &screen) {
    return r.left >= screen.left - 0.5f && r.top >= screen.top - 0.5f && r.right <= screen.right + 0.5f &&
           r.bottom <= screen.bottom + 0.5f && r.right > r.left && r.bottom > r.top;
}

}  // namespace

TEST(AppWallFollowsTheDisplays) {
    const std::vector<MonitorInfo> two = TwoDisplays();
    Settings s;
    s.monitorMode = MonitorMode::Span;
    // The wall is 3200 x 1080: a picture 640 wide is 216 tall.
    CHECK(app::PictureHeight(s, two, &two[0], 640) == 216);
    std::vector<app::Screen> wall = app::Wall(s, two, 640, 216);
    CHECK(wall.size() == 2);
    CHECK(wall[0].rect.left == 0 && wall[0].rect.top == 0 && wall[0].rect.right == 384 && wall[0].rect.bottom == 216);
    CHECK(wall[1].rect.left == 384 && wall[1].rect.top == 11 && wall[1].rect.right == 640 && wall[1].rect.bottom == 216);
    // Components start on the main display, unless they are shown on every one.
    CHECK(wall[0].components && !wall[1].components);
    s.componentDisplays = ComponentDisplays::All;
    wall = app::Wall(s, two, 640, 216);
    CHECK(wall[0].components && wall[1].components);

    // Not spanning, or one display: the picture is the display it is shown on.
    s.monitorMode = MonitorMode::Duplicate;
    CHECK(app::Wall(s, two, 640, 216).empty() && app::PictureHeight(s, two, &two[1], 640) == 512);
    s.monitorMode = MonitorMode::Span;
    const std::vector<MonitorInfo> one{two[0]};
    CHECK(app::Wall(s, one, 640, 360).empty() && app::PictureHeight(s, one, &one[0], 640) == 360);
    CHECK(app::PictureHeight(s, {}, nullptr, 160) == 90);
}

TEST(AppPicturesDrawComponentsOnEachScreenOfTheWall) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OneThemeStores stores;
    app::Library library(stores);
    app::Pictures pictures(library);
    const theme::Theme t = library.Edited(kTheme);

    Settings s;
    s.monitorMode = MonitorMode::Span;
    s.componentDisplays = ComponentDisplays::All;
    const std::vector<app::Screen> wall = app::Wall(s, TwoDisplays(), 640, 216);
    Picture p;
    std::vector<app::Bound> bounds;
    CHECK(pictures.Theme(t, 640, 216, app::Draw::Everything, &p, &bounds, &wall));
    CHECK(p.width == 640 && p.height == 216 && p.bgra.size() == 640u * 216 * 4);
    bool on[2] = {false, false};
    for (const app::Bound &b : bounds) {
        CHECK(b.instance == L"clock" && b.screen < 2);
        if (b.screen >= 2) continue;
        on[b.screen] = true;
        CHECK(Within(b.rect, wall[b.screen].rect));
    }
    CHECK(on[0] && on[1]);

    // Only the main display shows them.
    std::vector<app::Screen> mainOnly = wall;
    mainOnly[1].components = false;
    bounds.clear();
    CHECK(pictures.Theme(t, 640, 216, app::Draw::Everything, &p, &bounds, &mainOnly));
    CHECK(!bounds.empty());
    for (const app::Bound &b : bounds) CHECK(b.screen == 0 && Within(b.rect, mainOnly[0].rect));
    if (SUCCEEDED(co)) CoUninitialize();
}

TEST(AppPicturesLayerIsTheComponentsAlone) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OneThemeStores stores;
    app::Library library(stores);
    app::Pictures pictures(library);
    Picture layer;
    std::vector<app::Bound> bounds;
    CHECK(pictures.Theme(library.Edited(kTheme), 320, 180, app::Draw::ComponentsOnly, &layer, &bounds));
    CHECK(layer.width == 320 && layer.height == 180 && !bounds.empty());
    // Nothing at the top right, far from the clock at the bottom left; something where it is.
    auto alpha = [&](UINT x, UINT y) { return layer.bgra[((size_t)y * layer.width + x) * 4 + 3]; };
    CHECK(alpha(315, 4) == 0 && alpha(200, 20) == 0);
    unsigned covered = 0;
    for (const app::Bound &b : bounds)
        if (b.key == L"p1")
            for (UINT y = (UINT)b.rect.top; y < (UINT)b.rect.bottom && y < layer.height; ++y)
                for (UINT x = (UINT)b.rect.left; x < (UINT)b.rect.right && x < layer.width; ++x) covered += alpha(x, y) > 0;
    CHECK(covered > 0);
    // Premultiplied: no colour brighter than its coverage.
    bool premultiplied = true;
    for (size_t i = 0; i < layer.bgra.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) premultiplied = premultiplied && layer.bgra[i + c] <= layer.bgra[i + 3];
    CHECK(premultiplied);
    const std::vector<uint8_t> png = app::Png(layer);
    CHECK(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    if (SUCCEEDED(co)) CoUninitialize();
}
