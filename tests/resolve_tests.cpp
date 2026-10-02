// settings.ini's theme keys, and what ResolveTheme makes of them for one display.
#include "check.h"

#include <map>
#include <string>

#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

using namespace animelogon;

namespace {

const std::wstring kMonitorA = L"00000000000000aa";
const std::wstring kMonitorB = L"00000000000000bb";
const std::wstring kMonitorC = L"00000000000000cc";
const std::wstring kMonitorD = L"00000000000000dd";
const std::wstring kVideoX = L"1111111111111111";
const std::wstring kVideoY = L"2222222222222222";
const std::wstring kImage = L"3333333333333333";
const std::string kImageText = "3333333333333333";
const std::wstring kThemeT = L"aaaaaaaaaaaaaaaa";
const std::wstring kThemeU = L"bbbbbbbbbbbbbbbb";
const std::wstring kBroken = L"cccccccccccccccc";
const std::wstring kBattery = L"0a7b0a7b0a7b0a7b";
const std::wstring kSkin = L"5555555555555555";

// The stores, in memory. Built-ins come from the real functions, which never touch the disk.
class FakeStore : public ThemeStore {
public:
    std::map<std::wstring, std::string> themes;      // theme.xml, installed form
    std::map<std::wstring, WallpaperInfo> wallpapers;
    std::map<std::wstring, std::string> components;  // component.xml

    bool ThemeExists(const std::wstring &id) const override { return id == kDefaultTheme || themes.count(id); }
    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
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
        const auto it = components.find(id);
        if (it == components.end()) return *why = L"no such component", false;
        return skin::Parse(it->second, component, why);
    }
};

WallpaperInfo Video(const std::wstring &id) {
    WallpaperInfo w;
    w.id = id;
    w.kind = WallpaperKind::Video;
    w.width = 1920;
    w.height = 1080;
    w.legacy = true;
    w.videoPath = L"C:\\ProgramData\\AnimeLogon\\videos\\" + id + L"\\video.mp4";
    return w;
}

WallpaperInfo Image(const std::wstring &id) {
    WallpaperInfo w;
    w.id = id;
    w.kind = WallpaperKind::Image;
    w.width = 3840;
    w.height = 2160;
    w.imagePath = L"C:\\ProgramData\\AnimeLogon\\wallpapers\\" + id + L"\\image.bmp";
    return w;
}

// A one-panel component with a colour and a size setting.
std::string Battery() {
    return "<component format=\"1\" name=\"Battery\"><settings>"
           "<color id=\"accent\" label=\"A\" default=\"#FFFFFF\"/>"
           "<number id=\"size\" label=\"S\" default=\"4\" min=\"2\" max=\"8\"/></settings>"
           "<panel anchor=\"top-right\" size=\"$size\"><line><text value=\"{time}\" color=\"$accent\"/></line></panel>"
           "</component>";
}

FakeStore Store() {
    FakeStore store;
    store.wallpapers[kVideoX] = Video(kVideoX);
    store.wallpapers[kVideoY] = Video(kVideoY);
    store.wallpapers[kImage] = Image(kImage);
    store.components[kBattery] = Battery();
    store.components[kSkin] = skin::DefaultText();
    store.themes[kThemeT] =
        "<theme format=\"1\" name=\"T\"><wallpaper ref=\"" + kImageText +
        "\" fit=\"fit\"/><component id=\"clock\" ref=\"clock\"><set key=\"color\" value=\"#111111\"/>"
        "<set key=\"size\" value=\"12\"/><set key=\"p1.anchor\" value=\"bottom-left\"/>"
        "<set key=\"bogus\" value=\"1\"/></component></theme>";
    store.themes[kThemeU] = "<theme format=\"1\" name=\"U\"><wallpaper ref=\"none\" fit=\"stretch\"/></theme>";
    store.themes[kBroken] = "<theme format=\"1\">";
    return store;
}

// Everything skin::Resolve gives, compared.
bool SameDrawing(const skin::Resolved &a, const skin::Resolved &b) {
    if (a.panels.size() != b.panels.size()) return false;
    for (size_t p = 0; p < a.panels.size(); ++p) {
        const skin::Panel &x = a.panels[p], &y = b.panels[p];
        if (x.anchor != y.anchor || x.size != y.size || x.margin != y.margin || x.offsetX != y.offsetX ||
            x.offsetY != y.offsetY || x.align != y.align || x.backdrop != y.backdrop ||
            x.backdropColor != y.backdropColor || x.lines.size() != y.lines.size() || x.shadows.size() != y.shadows.size())
            return false;
        for (size_t s = 0; s < x.shadows.size(); ++s) {
            const skin::Shadow &m = x.shadows[s], &n = y.shadows[s];
            if (m.blur != n.blur || m.opacity != n.opacity || m.x != n.x || m.y != n.y || m.color != n.color) return false;
        }
        for (size_t l = 0; l < x.lines.size(); ++l) {
            if (x.lines[l].gap != y.lines[l].gap || x.lines[l].texts.size() != y.lines[l].texts.size()) return false;
            for (size_t t = 0; t < x.lines[l].texts.size(); ++t) {
                const skin::Text &m = x.lines[l].texts[t], &n = y.lines[l].texts[t];
                if (m.value != n.value || m.font != n.font || m.size != n.size || m.weight != n.weight ||
                    m.italic != n.italic || m.color != n.color || m.opacity != n.opacity || m.tracking != n.tracking ||
                    m.space != n.space || m.textCase != n.textCase || m.hang != n.hang)
                    return false;
            }
        }
    }
    return true;
}

// The clock the overlay drew before themes (overlay.cpp): the skin `skin` names, or the default
// skin when that is refused, with the values kept for whichever was used.
skin::Resolved OldClock(const Settings &s, const FakeStore &store) {
    skin::Skin skin;
    std::wstring refused;
    if (!store.LoadComponent(s.clock.skin, &skin, &refused)) skin = skin::Default();
    const std::wstring &id = refused.empty() ? s.clock.skin : std::wstring(L"default");
    return skin::Resolve(skin, s.clock.ValuesFor(id));
}

}  // namespace

TEST(SettingsThemeKeysRoundTrip) {
    Settings s;
    s.theme = kThemeT;
    s.components = false;
    s.componentDisplays = ComponentDisplays::All;
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[kMonitorA] = L"default";
    s.screens[kMonitorB] = kThemeU;
    ThemeOverrides &o = s.themeOverrides[kThemeT];
    o.wallpaper = L"none";
    o.fit = Scaling::Stretch;
    o.instances[L"clock"].visible = false;
    o.instances[L"clock"].values[L"p1.offset-x"] = L"4.5";
    o.instances[L"clock"].values[L"accent"] = L"#FFD27F";
    o.instances[L"battery"].ref = kBattery;
    o.instances[L"battery"].visible = true;
    s.themeOverrides[L"default"].instances[L"clock"].values[L"font"] = L"Microsoft YaHei UI";
    s.themeOverrides[L"default"].wallpaper = kImage;
    // The legacy keys go on being written alongside.
    s.video = kVideoX;
    s.scaling = Scaling::Fit;
    s.clock.enabled = false;
    s.clock.displays = ClockDisplays::Primary;
    s.clock.skin = kSkin;
    s.clock.values[L"default"][L"color"] = L"#12ABEF";
    s.clock.style.hours = ClockHours::H24;

    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(t.theme && *t.theme == kThemeT);
    CHECK(t.components && !*t.components);
    CHECK(t.componentDisplays && *t.componentDisplays == ComponentDisplays::All);
    CHECK(t.screens.size() == 2 && t.screens.at(kMonitorA) == L"default" && t.screens.at(kMonitorB) == kThemeU);
    CHECK(t.themeOverrides.size() == 2);
    const ThemeOverrides &p = t.OverridesFor(kThemeT);
    CHECK(p.wallpaper && *p.wallpaper == L"none" && p.fit && *p.fit == Scaling::Stretch);
    CHECK(p.instances.size() == 2);
    const InstanceOverrides &clock = p.instances.at(L"clock");
    CHECK(!clock.ref && clock.visible && !*clock.visible && clock.values.size() == 2);
    CHECK(clock.values.at(L"p1.offset-x") == L"4.5" && clock.values.at(L"accent") == L"#FFD27F");
    const InstanceOverrides &battery = p.instances.at(L"battery");
    CHECK(battery.ref && *battery.ref == kBattery && battery.visible && *battery.visible && battery.values.empty());
    const ThemeOverrides &d = t.OverridesFor(L"default");
    CHECK(d.wallpaper && *d.wallpaper == kImage && !d.fit);
    CHECK(d.instances.at(L"clock").values.at(L"font") == L"Microsoft YaHei UI");
    CHECK(t.OverridesFor(kThemeU).instances.empty() && !t.OverridesFor(kThemeU).wallpaper);
    CHECK(t.video == kVideoX && t.scaling == Scaling::Fit && !t.clock.enabled);
    CHECK(t.clock.displays == ClockDisplays::Primary && t.clock.skin == kSkin);
    CHECK(t.clock.ValuesFor(L"default").at(L"color") == L"#12ABEF" && t.clock.style.hours == ClockHours::H24);
    CHECK(SerializeSettings(t) == SerializeSettings(s));
}

TEST(SettingsWithoutThemeKeysStayLegacy) {
    // What the old settings app writes has no theme keys, read or written.
    const std::wstring legacy = SerializeSettings(Settings{});
    CHECK(legacy.find(L"theme") == std::wstring::npos);
    CHECK(legacy.find(L"components") == std::wstring::npos && legacy.find(L"component_displays") == std::wstring::npos);
    Settings s = ParseSettings(L"video = 1111111111111111\r\nskin.default.color = #FFFFFF\r\nclock = false\r\n");
    CHECK(!s.theme && !s.components && !s.componentDisplays && s.themeOverrides.empty());
    s.video = kVideoY;  // the old app changes something and saves
    CHECK(SerializeSettings(s).find(L"theme") == std::wstring::npos);
    // Each theme key is kept once it was read.
    const Settings themed = ParseSettings(L"theme = default\r\n");
    CHECK(themed.theme && *themed.theme == L"default" && !themed.components);
    const std::wstring written = SerializeSettings(themed);
    CHECK(written.find(L"theme = default\r\n") != std::wstring::npos && written.find(L"components") == std::wstring::npos);
    const Settings shown = ParseSettings(L"components = true\r\n");
    CHECK(!shown.theme && shown.components && *shown.components);
    CHECK(SerializeSettings(shown).find(L"components = true\r\n") != std::wstring::npos);
}

TEST(SettingsRefuseBadThemeKeys) {
    std::vector<std::wstring> problems;
    const Settings s = ParseSettings(
        L"theme = Default\r\n"
        L"theme = ../x\r\n"
        L"theme =\r\n"
        L"components = maybe\r\n"
        L"component_displays = left\r\n"
        L"theme.aaaaaaaaaaaaaaaa = x\r\n"
        L"theme.nothex.wallpaper = default\r\n"
        L"theme.default.wallpaper = ../w\r\n"
        L"theme.default.wallpaper = wallpaper.mp4\r\n"
        L"theme.default.fit = cover\r\n"
        L"theme.default.Clock.color = #FFFFFF\r\n"
        L"theme.default.wallpaper.ref = clock\r\n"
        L"theme.default.fit.visible = true\r\n"
        L"theme.default.clock.ref = default\r\n"
        L"theme.default.clock.visible = maybe\r\n"
        L"theme.default.clock.p1.l1.t1.value = x\r\n"
        L"theme.default.clock.color = a\x01\r\n"
        L"theme.default.clock.color = " + std::wstring(129, L'x') + L"\r\n"
        L"theme.default.clock = 1\r\n"
        L"theme.default.clock.Size = 1\r\n"
        L"screen.00000000000000aa = Default\r\n"
        L"themes = 1\r\n"
        L"theme.default.clock.color = #FFFFFF\r\n",
        &problems);
    CHECK(problems.size() == 22);
    CHECK(!s.theme && !s.components && !s.componentDisplays && s.screens.empty());
    // Only the last line was good.
    CHECK(s.themeOverrides.size() == 1);
    const ThemeOverrides &d = s.OverridesFor(L"default");
    CHECK(!d.wallpaper && !d.fit && d.instances.size() == 1);
    const InstanceOverrides &clock = d.instances.at(L"clock");
    CHECK(!clock.ref && !clock.visible && clock.values.size() == 1 && clock.values.at(L"color") == L"#FFFFFF");
}

TEST(ResolveLegacyMatchesTheOldOverlay) {
    const FakeStore store = Store();
    Settings s;
    s.video = kVideoX;
    s.scaling = Scaling::Fit;
    s.clock.displays = ClockDisplays::All;
    s.clock.values[L"default"][L"color"] = L"#12ABEF";
    s.clock.values[L"default"][L"p1.l1.t1.weight"] = L"350";
    s.clock.values[L"default"][L"size"] = L"99";  // out of range: the old overlay ignored it too
    s.clock.values[kSkin][L"color"] = L"#000000";
    std::vector<std::wstring> problems;
    ResolvedTheme r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.legacy && r.themeId.empty());
    CHECK(r.wallpaper && r.wallpaper->id == kVideoX && r.wallpaper->kind == WallpaperKind::Video);
    CHECK(r.wallpaper && r.wallpaper->videoPath == store.wallpapers.at(kVideoX).videoPath);
    CHECK(r.fit == Scaling::Fit && r.componentDisplays == ComponentDisplays::All);
    CHECK(r.components.size() == 1);
    if (r.components.size() == 1) {
        CHECK(r.components[0].instance == L"clock" && r.components[0].component == L"clock");
        CHECK(r.components[0].values.size() == 2 && !r.components[0].values.count(L"size"));
        CHECK(SameDrawing(skin::Resolve(r.components[0].skin, r.components[0].values), OldClock(s, store)));
    }
    CHECK(problems.size() == 1);

    // An imported skin, with its own values.
    s.clock.skin = kSkin;
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.components.size() == 1 && r.components[0].component == kSkin);
    CHECK(r.components.size() == 1 && r.components[0].values.at(L"color") == L"#000000");
    CHECK(r.components.size() == 1 && SameDrawing(skin::Resolve(r.components[0].skin, r.components[0].values), OldClock(s, store)));
    // A skin that does not load: the default skin, with the default skin's values.
    s.clock.skin = L"6666666666666666";
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.components.size() == 1 && r.components[0].component == L"clock");
    CHECK(r.components.size() == 1 && r.components[0].values.at(L"color") == L"#12ABEF");
    CHECK(r.components.size() == 1 && SameDrawing(skin::Resolve(r.components[0].skin, r.components[0].values), OldClock(s, store)));
    CHECK(problems.size() == 2);
    // The clock switched off.
    s.clock.enabled = false;
    CHECK(ResolveTheme(s, kMonitorA, store).components.empty());
    // No video: nothing behind the clock, and the old overlay left that display alone.
    s.video.clear();
    CHECK(!ResolveTheme(s, kMonitorA, store).wallpaper);
    // A video that does not load is not shown either.
    s.video = L"7777777777777777";
    problems.clear();
    CHECK(!ResolveTheme(s, kMonitorA, store, &problems).wallpaper && problems.size() == 1);

    // Each display its own video, as Settings::VideoFor gives; spanning or duplicating ignores them.
    s.video = kVideoX;
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[kMonitorA] = kVideoY;
    for (const std::wstring &m : {kMonitorA, kMonitorB}) {
        r = ResolveTheme(s, m, store);
        CHECK(r.legacy && r.wallpaper && r.wallpaper->id == s.VideoFor(m));
    }
    CHECK(ResolveTheme(s, kMonitorA, store).wallpaper->id == kVideoY);
    for (MonitorMode mode : {MonitorMode::Span, MonitorMode::Duplicate}) {
        s.monitorMode = mode;
        CHECK(ResolveTheme(s, kMonitorA, store).wallpaper->id == kVideoX);
    }
}

TEST(ResolveOverridePrecedence) {
    const FakeStore store = Store();
    Settings s;
    s.theme = kThemeT;
    std::vector<std::wstring> problems;
    ResolvedTheme r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(!r.legacy && r.themeId == kThemeT);
    CHECK(r.wallpaper && r.wallpaper->id == kImage && r.wallpaper->kind == WallpaperKind::Image);
    CHECK(r.fit == Scaling::Fit && r.components.size() == 1);
    CHECK(problems.size() == 1);  // the theme's own `bogus` set
    if (r.components.size() == 1) {
        const skin::Values &v = r.components[0].values;
        CHECK(v.size() == 3 && v.at(L"color") == L"#111111" && v.at(L"size") == L"12" && !v.count(L"bogus"));
    }

    // settings.ini wins over the theme; a bad override is dropped and the theme's value stays.
    InstanceOverrides &clock = s.themeOverrides[kThemeT].instances[L"clock"];
    clock.values[L"color"] = L"#222222";
    clock.values[L"size"] = L"99";
    clock.values[L"p1.l1.t1.weight"] = L"600";
    clock.values[L"p1.l9.t1.weight"] = L"600";
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(problems.size() == 3);
    CHECK(r.components.size() == 1);
    if (r.components.size() == 1) {
        const skin::Values &v = r.components[0].values;
        CHECK(v.size() == 4 && v.at(L"color") == L"#222222" && v.at(L"size") == L"12");
        CHECK(v.at(L"p1.anchor") == L"bottom-left" && v.at(L"p1.l1.t1.weight") == L"600");
        const skin::Resolved drawn = skin::Resolve(r.components[0].skin, v);
        CHECK(drawn.panels.size() == 1 && drawn.panels[0].size == 12.0f && drawn.panels[0].anchor == 6);
        CHECK(drawn.panels.size() == 1 && drawn.panels[0].lines[0].texts[0].weight == 600);
        CHECK(drawn.panels.size() == 1 && drawn.panels[0].lines[0].texts[0].color == 0xFF222222);
    }

    // The wallpaper and the fit.
    s.themeOverrides[kThemeT].fit = Scaling::Stretch;
    s.themeOverrides[kThemeT].wallpaper = L"none";
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.fit == Scaling::Stretch && !r.wallpaper);
    s.themeOverrides[kThemeT].wallpaper = kVideoX;
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.wallpaper && r.wallpaper->id == kVideoX);
    // A wallpaper that does not load gives the built-in one.
    s.themeOverrides[kThemeT].wallpaper = L"7777777777777777";
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.wallpaper && r.wallpaper->builtIn && r.wallpaper->id == L"default");
    CHECK(problems.size() == 4);
}

TEST(ResolveInstances) {
    const FakeStore store = Store();
    Settings s;
    s.theme = kThemeT;
    ThemeOverrides &o = s.themeOverrides[kThemeT];
    // Added by settings.ini, with values of its own only.
    o.instances[L"battery"].ref = kBattery;
    o.instances[L"battery"].values[L"accent"] = L"#FFD27F";
    o.instances[L"battery"].values[L"color"] = L"#000000";  // the battery has no such setting
    // An override for an instance that is not there and names no component.
    o.instances[L"ghost"].values[L"size"] = L"4";
    std::vector<std::wstring> problems;
    ResolvedTheme r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.components.size() == 2);
    if (r.components.size() == 2) {
        CHECK(r.components[0].instance == L"clock" && r.components[1].instance == L"battery");
        CHECK(r.components[1].component == kBattery && r.components[1].skin.name == L"Battery");
        CHECK(r.components[1].values.size() == 1 && r.components[1].values.at(L"accent") == L"#FFD27F");
    }
    CHECK(problems.size() == 3);  // bogus, color, ghost

    // Another component in the clock's place: the theme's sets were for the clock.
    o.instances[L"clock"].ref = kBattery;
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.components.size() == 2 && r.components[0].component == kBattery && r.components[0].values.empty());
    CHECK(problems.size() == 2);  // color, ghost
    o.instances.erase(L"clock");

    // Hidden.
    o.instances[L"clock"].visible = false;
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.components.size() == 1 && r.components[0].instance == L"battery");
    // A component that does not load leaves its instance out.
    o.instances[L"battery"].ref = L"9999999999999999";
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.components.empty() && problems.size() == 2);  // the battery, the ghost
    // `components = false` hides every instance; component_displays is passed on.
    o.instances.clear();
    CHECK(ResolveTheme(s, kMonitorA, store).components.size() == 1);
    CHECK(ResolveTheme(s, kMonitorA, store).componentDisplays == ComponentDisplays::Auto);
    s.components = false;
    s.componentDisplays = ComponentDisplays::Primary;
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.components.empty() && r.componentDisplays == ComponentDisplays::Primary && r.wallpaper);
    // The legacy keys mean nothing once there is a theme.
    s.components = true;
    s.clock.enabled = false;
    s.clock.displays = ClockDisplays::All;
    s.scaling = Scaling::Stretch;
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.components.size() == 1 && r.componentDisplays == ComponentDisplays::Primary && r.fit == Scaling::Fit);
}

TEST(ResolvePerMonitorSelection) {
    FakeStore store = Store();
    Settings s;
    s.theme = kThemeT;
    s.video = kVideoX;
    s.monitorMode = MonitorMode::PerMonitor;
    s.screens[kMonitorA] = kThemeU;
    s.screens[kMonitorB] = L"default";
    s.screens[kMonitorC] = kVideoY;  // a legacy video id names no theme
    std::vector<std::wstring> problems;
    CHECK(ResolveTheme(s, kMonitorA, store).themeId == kThemeU);
    CHECK(!ResolveTheme(s, kMonitorA, store).wallpaper && ResolveTheme(s, kMonitorA, store).fit == Scaling::Stretch);
    CHECK(ResolveTheme(s, kMonitorB, store).themeId == L"default");
    // Reported: the screen naming no theme, and theme T's own bad set.
    CHECK(ResolveTheme(s, kMonitorC, store, &problems).themeId == kThemeT && problems.size() == 2);
    CHECK(ResolveTheme(s, kMonitorD, store).themeId == kThemeT);
    // Duplicating and spanning use `theme` everywhere.
    for (MonitorMode mode : {MonitorMode::Duplicate, MonitorMode::Span}) {
        s.monitorMode = mode;
        for (const std::wstring &m : {kMonitorA, kMonitorB, kMonitorC, kMonitorD})
            CHECK(ResolveTheme(s, m, store).themeId == kThemeT);
    }
    // Without `theme`, a display whose screen names a theme shows it and the others stay legacy.
    s.theme.reset();
    s.monitorMode = MonitorMode::PerMonitor;
    CHECK(ResolveTheme(s, kMonitorA, store).themeId == kThemeU && !ResolveTheme(s, kMonitorA, store).legacy);
    CHECK(ResolveTheme(s, kMonitorB, store).themeId == L"default");
    ResolvedTheme r = ResolveTheme(s, kMonitorC, store);
    CHECK(r.legacy && r.wallpaper && r.wallpaper->id == kVideoY);
    r = ResolveTheme(s, kMonitorD, store);
    CHECK(r.legacy && r.wallpaper && r.wallpaper->id == kVideoX);
    // A theme that is there but does not load gives the built-in one, with its overrides.
    s.screens[kMonitorD] = kBroken;
    s.themeOverrides[L"default"].fit = Scaling::Fit;
    problems.clear();
    r = ResolveTheme(s, kMonitorD, store, &problems);
    CHECK(!r.legacy && r.themeId == L"default" && r.fit == Scaling::Fit && problems.size() == 1);
}

TEST(ResolveDefaultTheme) {
    const FakeStore store = Store();
    Settings s;
    s.theme = L"default";
    std::vector<std::wstring> problems;
    ResolvedTheme r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(problems.empty());
    CHECK(!r.legacy && r.themeId == L"default" && r.fit == Scaling::Fill);
    CHECK(r.wallpaper && r.wallpaper->builtIn && r.wallpaper->kind == WallpaperKind::Image);
    CHECK(r.components.size() == 1 && r.components[0].instance == L"clock" && r.components[0].component == L"clock");
    CHECK(r.components.size() == 1 && r.components[0].values.empty());
    CHECK(r.components.size() == 1 &&
          SameDrawing(skin::Resolve(r.components[0].skin, r.components[0].values), skin::Resolve(skin::Default(), {})));
    s.themeOverrides[L"default"].instances[L"clock"].values[L"color"] = L"#12ABEF";
    r = ResolveTheme(s, kMonitorA, store);
    CHECK(r.components.size() == 1 && r.components[0].values.at(L"color") == L"#12ABEF");
    // A theme id that names nothing: the built-in theme.
    s.theme = L"dddddddddddddddd";
    problems.clear();
    r = ResolveTheme(s, kMonitorA, store, &problems);
    CHECK(r.themeId == L"default" && problems.size() == 1);
}
