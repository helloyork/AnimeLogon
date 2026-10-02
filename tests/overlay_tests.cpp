// The overlay's plan of what each display shows, and how it reads still wallpapers.
#include "check.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "animelogon/background.h"
#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"
#include "layout.h"
#include "picture.h"
#include "plan.h"

using namespace animelogon;

namespace {

const std::wstring kVideoX = L"1111111111111111";
const std::wstring kVideoY = L"2222222222222222";
const std::wstring kImage = L"3333333333333333";
const std::string kImageText = "3333333333333333";
const std::wstring kThemeT = L"aaaaaaaaaaaaaaaa";  // the image, fit, the clock moved
const std::wstring kThemeU = L"bbbbbbbbbbbbbbbb";  // no wallpaper, no components
const std::wstring kThemeV = L"cccccccccccccccc";  // the video Y, two clocks

// The stores, in memory, counting how often a theme is loaded. Built-ins come from the real
// functions, which never touch the disk.
class FakeStore : public ThemeStore {
public:
    std::map<std::wstring, std::string> themes;
    std::map<std::wstring, WallpaperInfo> wallpapers;
    mutable int themeLoads = 0;

    bool ThemeExists(const std::wstring &id) const override { return id == kDefaultTheme || themes.count(id); }
    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        ++themeLoads;
        if (id == kDefaultTheme) return ::animelogon::LoadTheme(id, t, why);
        const auto it = themes.find(id);
        if (it == themes.end()) return *why = L"no such theme", false;
        return theme::Parse(it->second, theme::Form::Installed, t, why);
    }
    bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) const override {
        if (id == kDefaultWallpaper) return ::animelogon::LoadWallpaper(id, info, why);
        const auto it = wallpapers.find(id);
        if (it == wallpapers.end()) return *why = L"no such wallpaper", false;
        *info = it->second;
        return true;
    }
    bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) const override {
        if (id == kClockComponent || id == L"default") return ::animelogon::LoadComponent(id, component, why);
        return *why = L"no such component", false;
    }
};

WallpaperInfo Video(const std::wstring &id, bool sound) {
    WallpaperInfo w;
    w.id = id;
    w.kind = WallpaperKind::Video;
    w.width = 1920;
    w.height = 1080;
    w.hasAudio = sound;
    w.videoPath = L"C:\\ProgramData\\AnimeLogon\\wallpapers\\" + id + L"\\video.mp4";
    if (sound) w.audioPath = L"C:\\ProgramData\\AnimeLogon\\wallpapers\\" + id + L"\\audio.wav";
    return w;
}

FakeStore Store() {
    FakeStore store;
    store.wallpapers[kVideoX] = Video(kVideoX, true);
    store.wallpapers[kVideoY] = Video(kVideoY, false);
    WallpaperInfo image;
    image.id = kImage;
    image.kind = WallpaperKind::Image;
    image.width = 3840;
    image.height = 2160;
    image.imagePath = L"C:\\ProgramData\\AnimeLogon\\wallpapers\\" + kImage + L"\\image.bmp";
    store.wallpapers[kImage] = image;
    store.themes[kThemeT] = "<theme format=\"1\" name=\"T\"><wallpaper ref=\"" + kImageText +
                            "\" fit=\"fit\"/><component id=\"clock\" ref=\"clock\">"
                            "<set key=\"position\" value=\"bottom-left\"/></component></theme>";
    store.themes[kThemeU] = "<theme format=\"1\" name=\"U\"><wallpaper ref=\"none\" fit=\"stretch\"/></theme>";
    store.themes[kThemeV] = "<theme format=\"1\" name=\"V\"><wallpaper ref=\"2222222222222222\" fit=\"fill\"/>"
                            "<component id=\"clock\" ref=\"clock\"/>"
                            "<component id=\"corner\" ref=\"clock\"><set key=\"position\" value=\"bottom-right\"/>"
                            "<set key=\"size\" value=\"5\"/></component></theme>";
    return store;
}

MonitorInfo Monitor(const wchar_t *key, LONG left, LONG top, LONG right, LONG bottom, bool primary) {
    MonitorInfo m;
    m.key = key;
    m.rect = {left, top, right, bottom};
    m.primary = primary;
    return m;
}

// Two side by side, the primary on the left, and a third below them.
std::vector<MonitorInfo> Monitors(size_t n = 2) {
    std::vector<MonitorInfo> all = {Monitor(L"00000000000000aa", 0, 0, 1920, 1080, true),
                                    Monitor(L"00000000000000bb", 1920, 0, 3840, 1080, false),
                                    Monitor(L"00000000000000cc", 0, 1080, 2560, 2520, false)};
    all.resize(n);
    return all;
}

bool Same(const RECT &a, const RECT &b) { return EqualRect(&a, &b) != FALSE; }

std::wstring ScratchDir() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-test-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteAll(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = bytes.empty() || (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size());
    CloseHandle(h);
    return ok;
}

uint32_t Big32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// The filtered rows of background::Render()'s PNG: its IDAT holds stored deflate blocks only.
std::vector<uint8_t> RenderRows() {
    const std::vector<uint8_t> png = background::Render();
    std::vector<uint8_t> raw;
    for (size_t at = 8; at + 12 <= png.size();) {
        const uint32_t length = Big32(&png[at]);
        if (std::string((const char *)&png[at + 4], 4) == "IDAT") {
            size_t z = at + 8 + 2;  // after the zlib header
            for (bool last = false; !last;) {
                last = png[z] & 1;
                const size_t n = png[z + 1] | (size_t)png[z + 2] << 8;
                raw.insert(raw.end(), png.begin() + (std::ptrdiff_t)z + 5, png.begin() + (std::ptrdiff_t)(z + 5 + n));
                z += 5 + n;
            }
        }
        at += 12 + length;
    }
    return raw;
}

}  // namespace

TEST(PlanLegacyIsTheOldPlan) {
    const FakeStore store = Store();
    Settings s;
    s.video = kVideoX;
    s.scaling = Scaling::Fit;
    plan::Plan p = plan::Build(s, Monitors(), store);
    CHECK(p.displays.size() == 2 && p.componentDisplays == ComponentDisplays::Auto);
    for (const plan::Display &d : p.displays) {
        CHECK(d.legacy && d.themeId.empty() && d.covered);
        CHECK(d.wallpaper.source == plan::Source::Video && d.wallpaper.id == kVideoX);
        CHECK(d.wallpaper.path == store.wallpapers.at(kVideoX).videoPath);
        CHECK(d.wallpaper.audioPath == store.wallpapers.at(kVideoX).audioPath && !d.wallpaper.audioPath.empty());
        CHECK(d.wallpaper.fit == Scaling::Fit && Same(d.canvas, d.rect));
        CHECK(d.components.size() == 1 && d.components[0].key == L"legacy/clock");
    }
    // The clock starts on the primary display only (Auto), as before.
    CHECK(p.displays[0].primary && p.displays[0].showComponents && !p.displays[1].showComponents);
    s.clock.displays = ClockDisplays::All;
    p = plan::Build(s, Monitors(), store);
    CHECK(p.componentDisplays == ComponentDisplays::All && p.displays[1].showComponents);
    s.clock.enabled = false;
    CHECK(plan::Build(s, Monitors(), store).displays[0].components.empty());

    // Each display its own video; spanning puts the primary's across both.
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[L"00000000000000bb"] = kVideoY;
    p = plan::Build(s, Monitors(), store);
    CHECK(p.displays[0].wallpaper.id == kVideoX && p.displays[1].wallpaper.id == kVideoY);
    CHECK(p.displays[1].wallpaper.audioPath.empty());
    s.monitorMode = MonitorMode::Span;
    p = plan::Build(s, Monitors(), store);
    const RECT wall{0, 0, 3840, 1080};
    for (const plan::Display &d : p.displays) CHECK(d.wallpaper.id == kVideoX && Same(d.canvas, wall));

    // Without a video, or with one that does not load, the displays are left to Windows.
    s.monitorMode = MonitorMode::Duplicate;
    s.video.clear();
    p = plan::Build(s, Monitors(), store);
    for (const plan::Display &d : p.displays) CHECK(!d.covered && d.wallpaper.source == plan::Source::None);
    s.video = L"7777777777777777";
    p = plan::Build(s, Monitors(), store);
    CHECK(!p.displays[0].covered && p.problems.size() == 1);
    CHECK(plan::Build(s, {}, store).displays.empty());
}

TEST(PlanThemeWallpapers) {
    const FakeStore store = Store();
    Settings s;
    s.theme = kThemeT;
    plan::Plan p = plan::Build(s, Monitors(), store);
    CHECK(p.displays.size() == 2);
    for (const plan::Display &d : p.displays) {
        CHECK(!d.legacy && d.themeId == kThemeT && d.covered);
        CHECK(d.wallpaper.source == plan::Source::Image && d.wallpaper.id == kImage);
        CHECK(d.wallpaper.path == store.wallpapers.at(kImage).imagePath && d.wallpaper.fit == Scaling::Fit);
        CHECK(d.components.size() == 1 && d.components[0].key == L"theme " + kThemeT + L"/clock");
        // The theme's own set moved it.
        CHECK(d.components.size() == 1 && d.components[0].drawing.panels.size() == 1 &&
              d.components[0].drawing.panels[0].anchor == 6);
    }
    // "none": still covered, black, and nothing to draw over it.
    s.theme = kThemeU;
    p = plan::Build(s, Monitors(), store);
    CHECK(p.displays[0].covered && p.displays[0].wallpaper.source == plan::Source::None);
    CHECK(p.displays[0].components.empty() && p.displays[0].wallpaper.fit == Scaling::Stretch);
    // The built-in theme: the gradient.
    s.theme = L"default";
    p = plan::Build(s, Monitors(), store);
    CHECK(p.displays[0].wallpaper.source == plan::Source::Gradient && p.displays[0].wallpaper.id == L"default");
    CHECK(p.displays[0].wallpaper.path.empty() && p.displays[0].components.size() == 1);
    // A video, two instances of one component.
    s.theme = kThemeV;
    p = plan::Build(s, Monitors(), store);
    CHECK(p.displays[0].wallpaper.source == plan::Source::Video && p.displays[0].wallpaper.id == kVideoY);
    CHECK(p.displays[0].components.size() == 2);
    if (p.displays[0].components.size() == 2) {
        const plan::Component &a = p.displays[0].components[0], &b = p.displays[0].components[1];
        CHECK(a.instance == L"clock" && b.instance == L"corner" && a.component == L"clock" && b.component == L"clock");
        CHECK(a.key != b.key && a.drawing.panels[0].anchor == 1 && b.drawing.panels[0].anchor == 8);
        CHECK(a.drawing.panels[0].size == 10.0f && b.drawing.panels[0].size == 5.0f);
    }
    // A hidden instance is not drawn.
    s.themeOverrides[kThemeV].instances[L"corner"].visible = false;
    p = plan::Build(s, Monitors(), store);
    CHECK(p.displays[0].components.size() == 1 && p.displays[0].components[0].instance == L"clock");
}

TEST(PlanComponentDisplays) {
    const FakeStore store = Store();
    Settings s;
    s.theme = kThemeV;
    plan::Plan p = plan::Build(s, Monitors(3), store);
    CHECK(p.componentDisplays == ComponentDisplays::Auto);
    CHECK(p.displays[0].showComponents && !p.displays[1].showComponents && !p.displays[2].showComponents);
    s.componentDisplays = ComponentDisplays::All;
    p = plan::Build(s, Monitors(3), store);
    for (const plan::Display &d : p.displays) CHECK(d.showComponents && d.components.size() == 2);
    s.componentDisplays = ComponentDisplays::Primary;
    p = plan::Build(s, Monitors(3), store);
    CHECK(p.displays[0].showComponents && !p.displays[2].showComponents);
    // The legacy clock keys mean nothing with a theme.
    s.clock.displays = ClockDisplays::All;
    s.clock.enabled = false;
    p = plan::Build(s, Monitors(3), store);
    CHECK(!p.displays[1].showComponents && p.displays[1].components.size() == 2);
    s.components = false;
    p = plan::Build(s, Monitors(3), store);
    for (const plan::Display &d : p.displays) CHECK(d.components.empty() && d.covered);
}

TEST(PlanMonitorModes) {
    FakeStore store = Store();
    Settings s;
    s.theme = kThemeT;
    // Spanning: the primary display's wallpaper across the wall; components per display.
    s.monitorMode = MonitorMode::Span;
    s.screens[L"00000000000000bb"] = kThemeV;  // ignored unless per-monitor
    plan::Plan p = plan::Build(s, Monitors(3), store);
    const RECT wall{0, 0, 3840, 2520};
    for (const plan::Display &d : p.displays) {
        CHECK(d.wallpaper.source == plan::Source::Image && d.wallpaper.id == kImage && Same(d.canvas, wall));
        CHECK(d.themeId == kThemeT && d.components.size() == 1 && !Same(d.rect, wall));
    }
    // Duplicating: the same theme everywhere, each display its own canvas; resolved once.
    s.monitorMode = MonitorMode::Duplicate;
    store.themeLoads = 0;
    p = plan::Build(s, Monitors(3), store);
    CHECK(store.themeLoads == 1);
    for (const plan::Display &d : p.displays) CHECK(d.themeId == kThemeT && Same(d.canvas, d.rect));
    // Per-monitor: each display its own theme; displays naming the same thing resolve once.
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[L"00000000000000aa"] = kThemeU;
    s.screens[L"00000000000000cc"] = kThemeU;
    store.themeLoads = 0;
    p = plan::Build(s, Monitors(3), store);
    CHECK(store.themeLoads == 2);
    CHECK(p.displays[0].themeId == kThemeU && p.displays[0].wallpaper.source == plan::Source::None);
    CHECK(p.displays[1].themeId == kThemeV && p.displays[1].wallpaper.id == kVideoY);
    CHECK(p.displays[2].themeId == kThemeU && Same(p.displays[2].canvas, p.displays[2].rect));
    CHECK(p.displays[1].components.size() == 2 && p.displays[1].components[0].key == L"theme " + kThemeV + L"/clock");
    // A display with no screen of its own shows `theme`; without `theme`, it stays legacy.
    s.screens.erase(L"00000000000000cc");
    p = plan::Build(s, Monitors(3), store);
    CHECK(p.displays[2].themeId == kThemeT && p.displays[2].wallpaper.source == plan::Source::Image);
    s.theme.reset();
    s.video = kVideoX;
    p = plan::Build(s, Monitors(3), store);
    CHECK(p.displays[2].legacy && p.displays[2].wallpaper.id == kVideoX && p.displays[2].components[0].key == L"legacy/clock");
    CHECK(!p.displays[0].legacy && p.displays[0].covered);
    // The log line names the theme, the wallpaper and the components.
    CHECK(plan::Describe(p.displays[1]).find(L"theme " + kThemeV) != std::wstring::npos);
    CHECK(plan::Describe(p.displays[1]).find(L"video " + kVideoY) != std::wstring::npos);
    CHECK(plan::Describe(p.displays[1]).find(L"2 component(s)") != std::wstring::npos);
    CHECK(plan::Describe(p.displays[2]).find(L"legacy") != std::wstring::npos);
}

TEST(GradientIsTheInstalledBackground) {
    // At the installed size, exactly the picture Render() encodes.
    const std::vector<uint8_t> rows = RenderRows();
    const std::vector<uint8_t> pixels = background::Pixels(background::kWidth, background::kHeight);
    const size_t stride = 1 + (size_t)background::kWidth * 3;
    CHECK(rows.size() == stride * background::kHeight);
    CHECK(pixels.size() == (size_t)background::kWidth * background::kHeight * 4);
    bool same = rows.size() == stride * background::kHeight;
    for (int y = 0; same && y < background::kHeight; ++y)
        for (int x = 0; same && x < background::kWidth; ++x) {
            const uint8_t *rgb = &rows[y * stride + 1 + (size_t)x * 3];
            const uint8_t *bgra = &pixels[((size_t)y * background::kWidth + x) * 4];
            same = rows[y * stride] == 0 && bgra[0] == rgb[2] && bgra[1] == rgb[1] && bgra[2] == rgb[0] && bgra[3] == 255;
        }
    CHECK(same);
    // At another size, the same colours from top to bottom, every column alike.
    const std::vector<uint8_t> big = background::Pixels(3, 2160);
    CHECK(big.size() == 3 * 2160 * 4);
    CHECK(big[0] == 28 && big[1] == 20 && big[2] == 18 && big[3] == 255);
    const size_t last = (size_t)2159 * 3 * 4;
    CHECK(big[last] == 10 && big[last + 1] == 7 && big[last + 2] == 6);
    CHECK(std::equal(big.begin(), big.begin() + 4, big.begin() + 8));
    CHECK(background::Pixels(0, 10).empty() && background::Pixels(4, 1).size() == 16);
    // The overlay draws one column, as tall as the canvas up to a limit.
    const picture::Image column = picture::GradientColumn(1080);
    CHECK(column.width == 1 && column.height == 1080 && column.bgra == background::Pixels(1, 1080));
    CHECK(picture::GradientColumn(20000).height == picture::kMaxGradientRows);
    CHECK(picture::GradientColumn(0).height == 1);
}

TEST(ReadImageTakesOnlyCanonicalBitmaps) {
    const std::wstring dir = ScratchDir();
    // 3 x 2, with a fourth byte that is not 255: the file says 255.
    std::vector<uint8_t> bgra(3 * 2 * 4);
    for (size_t i = 0; i < bgra.size(); ++i) bgra[i] = (uint8_t)(i * 7 + 1);
    const std::vector<uint8_t> file = bitmap::Build(3, 2, bgra.data(), bgra.size());
    CHECK(file.size() == bitmap::kHeaderBytes + bgra.size());
    const std::wstring good = dir + L"\\image.bmp";
    CHECK(WriteAll(good, file));
    picture::Image image;
    std::wstring why;
    CHECK(picture::ReadImage(good, &image, &why));
    CHECK(image.width == 3 && image.height == 2);
    CHECK(image.bgra.size() == bgra.size() &&
          std::equal(image.bgra.begin(), image.bgra.end(), file.begin() + bitmap::kHeaderBytes));
    CHECK(image.bgra.size() == bgra.size() && image.bgra[3] == 255 && image.bgra[0] == bgra[0]);

    auto refused = [&](const std::vector<uint8_t> &bytes) {
        const std::wstring path = dir + L"\\bad.bmp";
        WriteAll(path, bytes);
        picture::Image out;
        out.width = 99;
        std::wstring reason;
        const bool read = picture::ReadImage(path, &out, &reason);
        return !read && out.width == 99 && !reason.empty();
    };
    std::vector<uint8_t> longer = file;
    longer.push_back(0);
    CHECK(refused(longer));
    std::vector<uint8_t> shorter(file.begin(), file.end() - 1);
    CHECK(refused(shorter));
    std::vector<uint8_t> upright = file;  // a positive height: rows bottom to top
    upright[22] = 2, upright[23] = upright[24] = upright[25] = 0;
    CHECK(refused(upright));
    std::vector<uint8_t> packed = file;  // 24-bit
    packed[28] = 24;
    CHECK(refused(packed));
    CHECK(refused({}));
    CHECK(refused(std::vector<uint8_t>(file.begin(), file.begin() + bitmap::kHeaderBytes)));

    picture::Image none;
    CHECK(!picture::ReadImage(dir + L"\\missing.bmp", &none, &why) && !why.empty());
    CHECK(!picture::ReadImage(dir, &none, &why));
    CHECK(!picture::ReadImage(L"relative\\image.bmp", &none, &why));
    CHECK(secure::RemoveTree(dir) == ERROR_SUCCESS);
}

TEST(ComposePlacesAnImageAsTheShaderDoes) {
    // 2 x 1: red, blue.
    picture::Image image;
    image.width = 2;
    image.height = 1;
    image.bgra = {0, 0, 255, 255, 255, 0, 0, 255};
    const RECT window{0, 0, 4, 4};
    // Fit: the image is 4 x 2 in the middle of the 4 x 4 window, with black above and below.
    const layout::Mapping fit = layout::Map(2, 1, window, window, Scaling::Fit);
    const std::vector<uint8_t> a = picture::Compose(image, fit, 4, 4, 4, 4);
    CHECK(a.size() == 4 * 4 * 3);
    auto at = [&](const std::vector<uint8_t> &bgr, int x, int y, int c) { return bgr[((size_t)y * 4 + x) * 3 + c]; };
    CHECK(at(a, 0, 0, 0) == 0 && at(a, 0, 0, 1) == 0 && at(a, 0, 0, 2) == 0);
    CHECK(at(a, 3, 3, 0) == 0 && at(a, 3, 3, 2) == 0);
    CHECK(at(a, 0, 1, 2) == 255 && at(a, 0, 1, 0) == 0);    // the red end
    CHECK(at(a, 3, 2, 0) == 255 && at(a, 3, 2, 2) == 0);    // the blue end
    CHECK(at(a, 1, 1, 2) > at(a, 1, 1, 0) && at(a, 2, 1, 0) > at(a, 2, 1, 2));  // blended between
    CHECK(picture::Compose(image, fit, 4, 4, 4, 4) == a);
    // Fill and stretch cover the whole window.
    for (Scaling s : {Scaling::Fill, Scaling::Stretch}) {
        const std::vector<uint8_t> b = picture::Compose(image, layout::Map(2, 1, window, window, s), 4, 4, 4, 4);
        bool covered = true;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) covered = covered && (at(b, x, y, 0) != 0 || at(b, x, y, 2) != 0);
        CHECK(covered);
    }
    // Drawn smaller than the window, it samples the window at the smaller grid.
    const std::vector<uint8_t> half = picture::Compose(image, layout::Map(2, 1, window, window, Scaling::Stretch), 4, 4, 2, 2);
    CHECK(half.size() == 2 * 2 * 3 && half[2] > half[0] && half[3] > half[5]);
}
