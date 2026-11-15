// The new settings app's library (app/src/library.h) and its messages (json.h), against stores
// in memory.
#include "check.h"

#include <map>
#include <string>

#include "animelogon/components.h"
#include "animelogon/settings.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"
#include "animelogon/text.h"

#include "importable.h"
#include "json.h"
#include "library.h"
#include "livespectrum.h"
#include "staging.h"
#include "traymenu.h"

using namespace animelogon;

namespace {

const std::wstring kThemeT = L"aaaaaaaaaaaaaaaa";
const std::wstring kThemeU = L"bbbbbbbbbbbbbbbb";
const std::wstring kWallpaperX = L"1111111111111111";
const std::wstring kWallpaperY = L"2222222222222222";
const std::wstring kBattery = L"0a7b0a7b0a7b0a7b";

class FakeStore : public ThemeStore {
public:
    std::map<std::wstring, std::string> themes;
    std::map<std::wstring, std::string> components;
    std::map<std::wstring, WallpaperInfo> wallpapers;

    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        if (id == kDefaultTheme) return *t = theme::Default(), true;
        const auto it = themes.find(id);
        if (it == themes.end()) return false;
        return theme::Parse(it->second, theme::Form::Installed, t, why);
    }
    bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *) const override {
        const auto it = wallpapers.find(id);
        if (it == wallpapers.end()) return false;
        *info = it->second;
        return true;
    }
    bool LoadComponent(const std::wstring &id, skin::Skin *s, std::wstring *why) const override {
        if (id == kClockComponent) return *s = skin::Default(), true;
        const auto it = components.find(id);
        if (it == components.end()) return false;
        return skin::Parse(it->second, s, why);
    }
};

class FakeStores : public app::Stores {
public:
    FakeStore store;
    std::wstring settings = L"theme = " + kThemeT + L"\r\n";
    mutable std::wstring written;
    mutable int writes = 0;

    const ThemeStore &Store() const override { return store; }
    std::vector<ThemeEntry> Themes() const override {
        std::vector<ThemeEntry> list{{kDefaultTheme, L"默认", L"AnimeLogon", true}};
        for (const auto &[id, text] : store.themes) {
            theme::Theme t;
            if (theme::Parse(text, theme::Form::Installed, &t, nullptr)) list.push_back({id, t.name, t.author, false});
        }
        return list;
    }
    std::vector<WallpaperInfo> Wallpapers() const override {
        std::vector<WallpaperInfo> list;
        for (const auto &[id, w] : store.wallpapers) list.push_back(w);
        return list;
    }
    std::vector<ComponentEntry> Components() const override {
        std::vector<ComponentEntry> list{{kClockComponent, L"时钟", L"AnimeLogon", true}};
        for (const auto &[id, text] : store.components) list.push_back({id, L"Battery", L"", false});
        return list;
    }
    Settings ReadSettings() const override { return ParseSettings(settings); }
    bool WriteSettings(const std::wstring &text) const override {
        written = text;
        ++writes;
        return true;
    }
};

WallpaperInfo Image(const std::wstring &id) {
    WallpaperInfo w;
    w.id = id;
    w.kind = WallpaperKind::Image;
    w.width = 3840;
    w.height = 2160;
    return w;
}

// Theme T: wallpaper X, the clock at the bottom left. Theme U: wallpaper Y, the clock as it comes.
FakeStores Stores() {
    FakeStores s;
    s.store.wallpapers[kWallpaperX] = Image(kWallpaperX);
    s.store.wallpapers[kWallpaperY] = Image(kWallpaperY);
    s.store.themes[kThemeT] = "<theme format=\"1\" name=\"T\"><wallpaper ref=\"1111111111111111\" fit=\"fill\"/>"
                              "<component id=\"clock\" ref=\"clock\"><set key=\"p1.anchor\" value=\"bottom-left\"/>"
                              "</component></theme>";
    s.store.themes[kThemeU] = "<theme format=\"1\" name=\"U\"><wallpaper ref=\"2222222222222222\" fit=\"fill\"/>"
                              "<component id=\"clock\" ref=\"clock\"/></theme>";
    s.store.components[kBattery] =
        "<component format=\"1\" name=\"Battery\"><settings><color id=\"accent\" label=\"A\" default=\"#FFFFFF\"/>"
        "</settings><panel anchor=\"top-right\" size=\"4\"><line><text value=\"{time}\" color=\"$accent\"/></line>"
        "</panel></component>";
    return s;
}

const skin::Values &Own(const app::Library &l, const std::wstring &theme, const std::wstring &instance) {
    static const skin::Values none;
    const ThemeOverrides &o = l.settings().OverridesFor(theme);
    const auto it = o.instances.find(instance);
    return it == o.instances.end() ? none : it->second.values;
}

// One value settings.ini holds for an instance, or empty when it holds none.
std::wstring Set(const app::Library &l, const std::wstring &theme, const std::wstring &instance, const std::wstring &key) {
    const skin::Values &values = Own(l, theme, instance);
    const auto it = values.find(key);
    return it == values.end() ? L"" : it->second;
}

}  // namespace

// While the page may move things, the panels of the open theme that hold a spectrum are left out of
// its pictures (fully transparent, still in their places) and named, for the page to draw moving.
TEST(AppLeavesSpectraOutForThePageToMove) {
    auto stores = Stores();
    // The built-in spectrum, as an installed component (the stores here hold no built-in but the clock).
    skin::Skin builtIn;
    CHECK(BuiltInComponent(L"spectrum", &builtIn));
    stores.store.components[L"0c7c0c7c0c7c0c7c"] = skin::Normalize(builtIn);
    stores.store.components[L"0b7b0b7b0b7b0b7b"] =
        "<component format=\"1\" name=\"Two\"><panel><line><text value=\"{time}\"/></line></panel>"
        "<panel><line><text value=\"{date}\"/><spectrum/></line></panel></component>";
    stores.store.themes[kThemeU] = "<theme format=\"1\" name=\"U\"><wallpaper ref=\"2222222222222222\" fit=\"fill\"/>"
                                   "<component id=\"clock\" ref=\"clock\"/><component id=\"bars\" ref=\"0c7c0c7c0c7c0c7c\"/>"
                                   "<component id=\"two\" ref=\"0b7b0b7b0b7b0b7b\"/>"
                                   "<component id=\"hidden\" ref=\"0c7c0c7c0c7c0c7c\" visible=\"false\"/></theme>";
    app::Library library(stores);
    const app::LiveSpectrum spectra(library);
    const theme::Theme t = library.Edited(kThemeU);
    CHECK(spectra.Panels(t) == (std::vector<std::wstring>{L"bars.p1", L"two.p2"}));
    const theme::Theme without = spectra.Without(t);
    auto set = [&](const std::wstring &instance, const std::wstring &key) {
        for (const theme::Instance &i : without.components)
            if (i.id == instance) {
                const auto it = i.sets.find(key);
                return it == i.sets.end() ? std::wstring() : it->second;
            }
        return std::wstring(L"?");
    };
    CHECK(set(L"bars", L"p1.opacity") == L"0" && set(L"two", L"p2.opacity") == L"0");
    CHECK(set(L"two", L"p1.opacity").empty() && set(L"clock", L"p1.opacity").empty() && set(L"hidden", L"p1.opacity").empty());
    // The component takes the value, so the pictures leave the panel out.
    skin::Skin s;
    CHECK(library.LoadSkin(L"0c7c0c7c0c7c0c7c", &s) && skin::Accepts(s, L"p1.opacity", L"0"));
}

TEST(AppSceneEditsSaveUndoResetAndDescribeControls) {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH,temp);
    const std::wstring path=std::wstring(temp)+L"scene-values-"+RandomHex(6)+L".xml";
    const std::string xml="<scene format=\"1\" name=\"T\"><setting id=\"tint\" kind=\"color\" label=\"Tint\" default=\"#ff0000\"/>"
        "<base shader=\"sky.hlsl\"/><layer lottie=\"bird.json\"/></scene>";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(file != INVALID_HANDLE_VALUE); if (file == INVALID_HANDLE_VALUE) return;
    DWORD n=0; CHECK(WriteFile(file,xml.data(),(DWORD)xml.size(),&n,nullptr) && n == xml.size()); CloseHandle(file);
    struct Remove { std::wstring path; ~Remove() { DeleteFileW(path.c_str()); } } remove{path};
    auto stores=Stores(); auto &wp=stores.store.wallpapers[kWallpaperX]; wp.kind=WallpaperKind::Scene; wp.scenePath=path;
    app::Library library(stores); CHECK(library.Open(kThemeT));
    CHECK(library.SetValue(L"wallpaper",L"tint",L"#00ff00"));
    CHECK(library.SetValue(L"wallpaper",L"p1.size",L"25"));
    CHECK(!library.SetValue(L"wallpaper",L"p1.size",L"nan"));
    CHECK(library.Modifications(kThemeT) == 2 && library.Save());
    CHECK(ParseSettings(stores.written).OverridesFor(kThemeT).wallpaperValues.at(L"tint") == L"#00ff00");
    CHECK(library.Undo() && library.settings().OverridesFor(kThemeT).wallpaperValues.size() == 1);
    CHECK(library.Redo() && library.settings().OverridesFor(kThemeT).wallpaperValues.size() == 2);
    json::Writer writer; library.WriteOpen(writer); json::Value opened;
    CHECK(json::Parse(writer.text(),&opened));
    CHECK(opened[L"wallpaper"][L"kind"].Str() == L"scene");
    CHECK(opened[L"wallpaper"][L"scene"][L"settings"].size() == 1);
    CHECK(opened[L"wallpaper"][L"scene"][L"parts"].size() == 1);
    CHECK(library.ClearValues(L"wallpaper",{L"tint",L"p1.size"}) && library.Modifications(kThemeT) == 0);
    CHECK(library.SetValue(L"wallpaper",L"tint",L"#00ff00"));
    CHECK(library.SetWallpaper(kWallpaperY) && library.settings().OverridesFor(kThemeT).wallpaperValues.empty());
}

TEST(AppJsonReadsMessages) {
    json::Value v;
    CHECK(json::Parse(L" {\"op\":\"setMany\",\"values\":{\"p1.anchor\":\"top\",\"n\":-1.5e2},\"live\":true,"
                      L"\"keys\":[\"a\",\"\\u65f6\\n\"],\"none\":null} ",
                      &v));
    CHECK(v[L"op"].Str() == L"setMany");
    CHECK(v[L"values"][L"p1.anchor"].Str() == L"top");
    CHECK(v[L"values"][L"n"].Num() == -150);
    CHECK(v[L"live"].Bool());
    CHECK(v[L"keys"].size() == 2 && v[L"keys"][1].Str() == L"时\n");
    CHECK(v[L"none"].IsNull() && v[L"missing"].IsNull() && v[L"op"][L"x"].IsNull());
    CHECK(v[L"values"].Members().size() == 2);
    for (const wchar_t *bad : {L"{\"a\":1,}", L"[01]", L"{\"a\"}", L"\"tab\there\"", L"[1] 2", L"{'a':1}", L"tru"})
        CHECK(!json::Parse(bad, &v));
    std::wstring deep(40, L'[');
    CHECK(!json::Parse(deep + std::wstring(40, L']'), &v));
}

TEST(AppJsonWritesWhatItReads) {
    json::Writer w;
    w.BeginObject().Member(L"text", L"a\"b\\c\n\x01").Member(L"n", 0.1).Member(L"i", 7).Member(L"b", false);
    w.Key(L"list").BeginArray().String(L"x").Null().Number(1e21).EndArray().EndObject();
    json::Value v;
    CHECK(json::Parse(w.text(), &v));
    CHECK(v[L"text"].Str() == L"a\"b\\c\n\x01");
    CHECK(v[L"n"].Num() == 0.1 && v[L"i"].Num() == 7 && !v[L"b"].Bool(true));
    CHECK(v[L"list"].size() == 3 && v[L"list"][1].IsNull() && v[L"list"][2].Num() == 1e21);
    CHECK(w.text().find(L"0.1,") != std::wstring::npos);  // the shortest spelling
}

TEST(AppImportOpensTheFileDialogOnTheKindChosen) {
    using namespace app::importable;
    CHECK(KindNamed(L"video") == Kind::Video && KindNamed(L"picture") == Kind::Picture && KindNamed(L"package") == Kind::Package);
    // A message without a kind, or with one this version does not know, gets every kind.
    CHECK(KindNamed(L"") == Kind::Any && KindNamed(L"scene") == Kind::Any && KindNamed(L"Video") == Kind::Any);
    CHECK(Patterns(kPackages) == L"*.altheme");
    CHECK(Patterns(kVideos) == L"*.mp4;*.mkv;*.mov;*.webm;*.avi;*.m4v;*.wmv");

    const std::vector<Filter> video = ThemeFilters(Kind::Video);
    CHECK(video.size() == 2 && std::string(video[0].name) == "filter.videos" && video[0].patterns == Patterns(kVideos));
    CHECK(std::string(video[1].name) == "filter.all" && video[1].patterns == L"*.*");
    const std::vector<Filter> picture = ThemeFilters(Kind::Picture);
    CHECK(picture.size() == 2 && std::string(picture[0].name) == "filter.pictures" && picture[0].patterns == Patterns(kPictures));
    const std::vector<Filter> package = ThemeFilters(Kind::Package);
    CHECK(package.size() == 2 && std::string(package[0].name) == "filter.themes" && package[0].patterns == L"*.altheme");

    // Every kind at once, as before kinds were asked for: all of them first, then each alone.
    const std::vector<Filter> any = ThemeFilters(Kind::Any);
    CHECK(any.size() == 5 && std::string(any[0].name) == "filter.importable");
    CHECK(any[0].patterns == Patterns(kPackages) + L";" + Patterns(kPictures) + L";" + Patterns(kVideos));
    CHECK(any[0].patterns.find(L"alscene") == std::wstring::npos);  // scenes are not taken (staging::kTakesScenes)
    CHECK(std::string(any.back().name) == "filter.all");
    const std::vector<Filter> wallpaper = WallpaperFilters();
    CHECK(wallpaper.size() == 4 && wallpaper[0].patterns == Patterns(kPictures) + L";" + Patterns(kVideos));
    CHECK(wallpaper[0].patterns.find(L"altheme") == std::wstring::npos);
}

// The types listed under each kind are the ones the importer takes as that kind.
TEST(AppImportableTypesAreTheImportersOwn) {
    using namespace app::importable;
    for (const wchar_t *e : kPictures) CHECK(staging::LooksLikePicture(std::wstring(L"C:\\a.") + e));
    for (const wchar_t *e : kVideos) CHECK(!staging::LooksLikePicture(std::wstring(L"C:\\a.") + e));
}

TEST(AppLibraryKeepsOnlyRealChanges) {
    FakeStores s = Stores();
    app::Library l(s);
    CHECK(l.Open(kThemeT));
    // The theme already puts the clock at the bottom left: saying so again is not an override.
    CHECK(l.SetValue(L"clock", L"p1.anchor", L"bottom-left"));
    CHECK(Own(l, kThemeT, L"clock").empty());
    CHECK(l.SetValue(L"clock", L"p1.anchor", L"top"));
    CHECK(Own(l, kThemeT, L"clock").at(L"p1.anchor") == L"top");
    CHECK(l.Modifications(kThemeT) == 1);
    CHECK(l.SetValue(L"clock", L"p1.anchor", L"bottom-left"));
    CHECK(!l.settings().themeOverrides.count(kThemeT));
    // Numbers compare by value.
    CHECK(l.SetValue(L"clock", L"p1.offset-x", L"0.0"));
    CHECK(Own(l, kThemeT, L"clock").empty());
}

TEST(AppLibraryRefusesWhatTheComponentDoesNotTake) {
    FakeStores s = Stores();
    app::Library l(s);
    CHECK(!l.SetValue(L"clock", L"p1.anchor", L"top"));  // nothing open
    CHECK(l.Open(kThemeT));
    CHECK(!l.SetValue(L"clock", L"p1.anchor", L"nowhere"));
    CHECK(!l.SetValue(L"clock", L"p9.anchor", L"top"));
    CHECK(!l.SetValue(L"clock", L"size", L"99"));
    CHECK(!l.SetValue(L"battery", L"accent", L"#FF0000"));
    CHECK(!l.SetValue(L"clock", L"color", L"#12345\n"));
    CHECK(!l.CanUndo());
}

TEST(AppLibrarySettingBringsBackWhatItFeeds) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(l.SetValue(L"clock", L"p1.size", L"12"));
    CHECK(l.SetValue(L"clock", L"size", L"14"));
    CHECK(!Own(l, kThemeT, L"clock").count(L"p1.size"));
    CHECK(Own(l, kThemeT, L"clock").at(L"size") == L"14");
    // Putting a panel's position back also puts back the setting that placed it.
    CHECK(l.SetValue(L"clock", L"position", L"top-left"));
    CHECK(l.ClearValues(L"clock", {L"p1.anchor"}));
    CHECK(!Own(l, kThemeT, L"clock").count(L"position"));
}

TEST(AppLibraryUndoesAGestureAsOneStep) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(l.SetValue(L"clock", L"p1.offset-x", L"1", 7));
    CHECK(l.SetValue(L"clock", L"p1.offset-x", L"2", 7));
    CHECK(l.SetValue(L"clock", L"p1.offset-y", L"3", 7));
    CHECK(l.SetValue(L"clock", L"color", L"#FFD27A"));
    CHECK(l.Undo());
    CHECK(!Own(l, kThemeT, L"clock").count(L"color"));
    CHECK(Own(l, kThemeT, L"clock").at(L"p1.offset-x") == L"2");
    CHECK(l.Undo());
    CHECK(Own(l, kThemeT, L"clock").empty());
    CHECK(!l.Undo());
    CHECK(l.Redo() && l.Redo());
    CHECK(Own(l, kThemeT, L"clock").at(L"color") == L"#FFD27A");
    CHECK(!l.Redo());
    CHECK(l.Reset() && !l.settings().themeOverrides.count(kThemeT));
    CHECK(l.Undo() && Own(l, kThemeT, L"clock").size() == 3);
}

TEST(AppLibraryAddsHidesAndRemovesInstances) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(l.SetVisible(L"clock", false));
    CHECK(l.settings().OverridesFor(kThemeT).instances.at(L"clock").visible == false);
    CHECK(l.SetVisible(L"clock", true));
    CHECK(!l.settings().themeOverrides.count(kThemeT));
    const std::wstring second = l.AddInstance(kClockComponent);
    const std::wstring battery = l.AddInstance(kBattery);
    CHECK(second == L"clock-2");
    CHECK(battery == L"c0a7b0a7b");
    CHECK(l.Edited(kThemeT).components.size() == 3);
    CHECK(l.SetValue(battery, L"accent", L"#FF0000"));
    CHECK(!l.RemoveInstance(L"clock"));  // the theme's own can only be hidden
    CHECK(l.RemoveInstance(second));
    CHECK(l.Edited(kThemeT).components.size() == 2);
    for (int i = 0; i < 10; ++i) l.AddInstance(kClockComponent);
    CHECK(l.Edited(kThemeT).components.size() == theme::kMaxInstances);
    CHECK(l.AddInstance(kClockComponent).empty());
}

TEST(AppLibraryCopiesComponentsToOtherThemes) {
    FakeStores s = Stores();
    // Theme V has components of its own: a clock in green, and two batteries.
    const std::wstring kThemeV = L"cccccccccccccccc";
    s.store.themes[kThemeV] = "<theme format=\"1\" name=\"V\"><wallpaper ref=\"default\" fit=\"fill\"/>"
                              "<component id=\"clock\" ref=\"clock\"><set key=\"color\" value=\"#00FF00\"/></component>"
                              "<component id=\"b1\" ref=\"0a7b0a7b0a7b0a7b\"/><component id=\"b2\" ref=\"0a7b0a7b0a7b0a7b\"/>"
                              "</theme>";
    // U has its own wallpaper and clock edits already.
    s.settings += L"theme." + kThemeU + L".wallpaper = " + kWallpaperX + L"\r\ntheme." + kThemeU + L".clock.size = 14\r\n";
    app::Library l(s);
    CHECK(l.CopyComponents({kThemeU}) == 0);  // nothing open
    CHECK(l.Open(kThemeT));
    CHECK(l.SetValue(L"clock", L"p1.offset-x", L"3"));
    const std::wstring battery = l.AddInstance(kBattery);
    CHECK(l.SetValue(battery, L"accent", L"#FF0000"));
    const unsigned revision = l.Revision(kThemeU);
    // The open theme itself, one not installed and one named twice count once or not at all.
    CHECK(l.CopyComponents({kThemeU, kThemeV, kThemeT, L"dddddddddddddddd", kThemeU}) == 2);
    CHECK(l.Revision(kThemeU) > revision);

    // U shows T's components; its wallpaper stays, its own clock edit is gone.
    const theme::Theme u = l.Edited(kThemeU);
    CHECK(u.wallpaper == kWallpaperX);
    CHECK(u.components.size() == 2);
    const theme::Instance *uc = u.Find(L"clock");
    CHECK(uc && uc->sets == (skin::Values{{L"p1.anchor", L"bottom-left"}, {L"p1.offset-x", L"3"}}));
    const theme::Instance *ub = u.Find(battery);
    CHECK(ub && ub->ref == kBattery && ub->visible && ub->sets == (skin::Values{{L"accent", L"#FF0000"}}));

    // V's own instances take T's: its clock loses the green it was given, a battery takes T's, and
    // the battery left over is hidden, as the theme's own cannot be removed.
    const theme::Theme v = l.Edited(kThemeV);
    CHECK(v.components.size() == 3);
    const theme::Instance *vc = v.Find(L"clock");
    const skin::Skin clock = skin::Default();
    CHECK(vc && vc->sets.at(L"color") == skin::ValueOf(*clock.Find(L"color"), {}));
    CHECK(vc && vc->sets.at(L"p1.anchor") == L"bottom-left" && vc->sets.at(L"p1.offset-x") == L"3");
    const theme::Instance *b1 = v.Find(L"b1"), *b2 = v.Find(L"b2");
    CHECK(b1 && b1->visible && b1->sets == (skin::Values{{L"accent", L"#FF0000"}}));
    CHECK(b2 && !b2->visible);

    // T and its undo are as they were; the copies are saved with everything else.
    CHECK(l.Edited(kThemeT).components.size() == 2 && l.CanUndo());
    CHECK(l.Save());
    const Settings saved = ParseSettings(s.written);
    CHECK(saved.OverridesFor(kThemeU).wallpaper == kWallpaperX);
    theme::Theme ownV;
    CHECK(s.store.LoadTheme(kThemeV, &ownV, nullptr));
    const theme::Theme savedV = theme::Edited(ownV, saved.OverridesFor(kThemeV));
    CHECK(savedV.components.size() == 3 && savedV.Find(L"b2") && !savedV.Find(L"b2")->visible);
}

TEST(AppLibraryKeepsSettingsWithinTheirLimitWhenCopyingComponents) {
    FakeStores s = Stores();
    // A theme of 8 clocks with 100 values each: as overrides, about 60 KB for each theme given them.
    std::string big = "<theme format=\"1\" name=\"W\"><wallpaper ref=\"default\" fit=\"fill\"/>";
    for (int i = 1; i <= 8; ++i) {
        big += "<component id=\"i" + std::to_string(i) + "\" ref=\"clock\">";
        for (int k = 100; k < 200; ++k) big += "<set key=\"s" + std::to_string(k) + "\" value=\"" + std::string(40, 'a') + "\"/>";
        big += "</component>";
    }
    big += "</theme>";
    const std::wstring kThemeW = L"eeeeeeeeeeeeeeee";
    s.store.themes[kThemeW] = big;
    std::vector<std::wstring> targets;
    for (int i = 1; i <= 5; ++i) {
        const std::wstring id = L"f00000000000000" + std::to_wstring(i);
        s.store.themes[id] = "<theme format=\"1\" name=\"E\"><wallpaper ref=\"default\" fit=\"fill\"/></theme>";
        targets.push_back(id);
    }
    app::Library l(s);
    CHECK(l.Open(kThemeW) && l.Edited(kThemeW).components.size() == 8);
    CHECK(l.CopyComponents(targets) == -1);
    for (const std::wstring &id : targets) CHECK(!l.settings().themeOverrides.count(id));
    CHECK(l.CopyComponents({targets[0]}) == 1);
    CHECK(l.Edited(targets[0]).components.size() == 8);
    CHECK(ToUtf8(SerializeSettings(l.settings())).size() <= kMaxSettingsBytes);
}

TEST(AppLibraryWallpaperAndFit) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(!l.SetWallpaper(L"3333333333333333"));  // not installed
    CHECK(l.SetWallpaper(kWallpaperY));
    CHECK(l.Edited(kThemeT).wallpaper == kWallpaperY);
    CHECK(l.SetWallpaper(kWallpaperX));
    CHECK(!l.settings().themeOverrides.count(kThemeT));
    CHECK(l.SetFit(Scaling::Fit) && l.Edited(kThemeT).fit == Scaling::Fit);
    // Brightness: whole percents kept, the theme's own value clears the override, out of range refused.
    const int before = l.Modifications(kThemeT);
    CHECK(l.SetBrightness(0.69999f, 0) && l.Edited(kThemeT).brightness == 0.7f);
    CHECK(l.settings().themeOverrides.at(kThemeT).brightness == 0.7f && l.Modifications(kThemeT) == before + 1);
    CHECK(!l.SetBrightness(0.05f, 0) && !l.SetBrightness(1.5f, 0) && l.Edited(kThemeT).brightness == 0.7f);
    CHECK(l.SetBrightness(1.0f, 0) && !l.settings().themeOverrides.at(kThemeT).brightness);
    // Blur: tenths kept, none clears the override, out of range refused.
    CHECK(l.SetBlur(2.84f, 0) && l.Edited(kThemeT).blur == 2.8f && l.settings().themeOverrides.at(kThemeT).blur == 2.8f);
    CHECK(!l.SetBlur(-0.5f, 0) && !l.SetBlur(5.5f, 0) && l.Edited(kThemeT).blur == 2.8f);
    CHECK(l.SetBlur(0.0f, 0) && !l.settings().themeOverrides.at(kThemeT).blur);
    CHECK(l.SetWallpaper(kNoWallpaper) && l.Edited(kThemeT).wallpaper == kNoWallpaper);
}

TEST(AppLibraryAppliesForgetsAndMovesThemes) {
    FakeStores s = Stores();
    app::Library l(s);
    CHECK(l.Current() == kThemeT);
    CHECK(!l.Apply(L"cccccccccccccccc", L""));
    CHECK(l.Apply(kThemeU, L"") && l.settings().theme == kThemeU);
    CHECK(l.SetGlobal(L"monitorMode", L"per-monitor"));
    CHECK(l.Apply(kThemeT, L"00000000000000aa") && l.settings().screens.at(L"00000000000000aa") == kThemeT);
    l.Open(kThemeT);
    CHECK(l.SetValue(L"clock", L"color", L"#FF0000"));
    l.Move(kThemeT, L"dddddddddddddddd");
    CHECK(l.settings().screens.at(L"00000000000000aa") == L"dddddddddddddddd");
    CHECK(l.settings().themeOverrides.count(L"dddddddddddddddd") && !l.settings().themeOverrides.count(kThemeT));
    l.Forget(L"dddddddddddddddd");
    CHECK(l.settings().screens.empty() && l.settings().themeOverrides.empty());
    CHECK(l.Apply(kThemeT, L"all") && l.settings().theme == kThemeT);
}

TEST(AppLibrarySharedSettings) {
    FakeStores s = Stores();
    app::Library l(s);
    const unsigned before = l.Revision(kThemeU);
    CHECK(l.SetGlobal(L"clock.hours", L"12") && l.settings().clock.hours == ClockHours::H12);
    CHECK(l.Revision(kThemeU) > before);  // every theme's pictures change
    CHECK(!l.SetGlobal(L"clock.hours", L"13"));
    CHECK(l.SetGlobal(L"clock.language", L"ja-JP") && !l.SetGlobal(L"clock.language", L"xx-XX"));
    CHECK(l.SetGlobal(L"audio.volume", L"35") && l.settings().audio.volume == 35);
    CHECK(!l.SetGlobal(L"audio.volume", L"150"));
    CHECK(l.SetGlobal(L"components", L"false") && !l.settings().components);
    CHECK(!l.SetGlobal(L"components", L"maybe"));
    CHECK(!l.SetGlobal(L"screen.00000000000000aa", L"cccccccccccccccc"));
    CHECK(!l.SetGlobal(L"theme", kThemeU));  // not one of the shared settings
    CHECK(l.Save() && s.writes == 1);
    CHECK(s.written.find(L"clock_hours = 12") != std::wstring::npos);
    CHECK(s.written.find(L"audio_volume = 35") != std::wstring::npos);
}

TEST(AppLibraryFindsWhatOnlyOneThemeUses) {
    FakeStores s = Stores();
    s.store.themes[L"eeeeeeeeeeeeeeee"] = "<theme format=\"1\" name=\"V\"><wallpaper ref=\"2222222222222222\" fit=\"fill\"/>"
                                          "<component id=\"b\" ref=\"0a7b0a7b0a7b0a7b\"/></theme>";
    app::Library l(s);
    std::vector<std::wstring> wallpapers, components;
    l.OnlyUsedBy(kThemeT, &wallpapers, &components);
    CHECK(wallpapers == std::vector<std::wstring>{kWallpaperX} && components.empty());
    wallpapers.clear();
    l.OnlyUsedBy(L"eeeeeeeeeeeeeeee", &wallpapers, &components);
    CHECK(wallpapers.empty());  // theme U shows it too
    CHECK(components == std::vector<std::wstring>{kBattery});
}

TEST(AppLibraryDescribesTheOpenTheme) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(l.SetValue(L"clock", L"p1.anchor", L"top"));
    json::Writer w;
    l.WriteOpen(w);
    json::Value v;
    CHECK(json::Parse(w.text(), &v));
    CHECK(v[L"id"].Str() == kThemeT && v[L"name"].Str() == L"T" && v[L"canUndo"].Bool());
    CHECK(v[L"wallpaper"][L"kind"].Str() == L"image" && v[L"room"].Num() == 7);
    const json::Value &clock = v[L"instances"][0];
    CHECK(clock[L"label"].Str() == L"时钟" && clock[L"base"].Bool() && clock[L"panels"].Num() == 1);
    bool positionFolded = false, colorFolded = true;
    for (const json::Value &setting : clock[L"settings"].Items()) {
        if (setting[L"id"].Str() == L"position") positionFolded = setting[L"folded"].Bool();
        if (setting[L"id"].Str() == L"color") colorFolded = setting[L"folded"].Bool();
    }
    CHECK(positionFolded && !colorFolded);
    const json::Value &panel = clock[L"parts"][0];
    CHECK(panel[L"key"].Str() == L"p1" && panel[L"modified"].Bool());
    CHECK(panel[L"adjustments"][0][L"key"].Str() == L"p1.anchor" && panel[L"adjustments"][0][L"value"].Str() == L"top");
    CHECK(panel[L"adjustments"][0][L"options"].size() == 9);

    json::Writer library;
    library.BeginObject();
    l.WriteLibrary(library);
    library.EndObject();
    CHECK(json::Parse(library.text(), &v));
    CHECK(v[L"current"].Str() == kThemeT && v[L"themes"].size() == 3);
    CHECK(v[L"themes"][1][L"modifications"].Num() == 1 && v[L"themes"][1][L"summary"].Str() == L"时钟");
    CHECK(v[L"wallpapers"][0][L"id"].Str() == L"default" && v[L"wallpapers"][3][L"id"].Str() == L"none");
}

namespace {

// The open theme as the page is told it, and one of its instances, setting or part by id or key.
json::Value Opened(const app::Library &l) {
    json::Writer w;
    l.WriteOpen(w);
    json::Value v;
    CHECK(json::Parse(w.text(), &v));
    return v;
}
const json::Value *ById(const json::Value &list, const wchar_t *member, const std::wstring &id) {
    for (const json::Value &item : list.Items())
        if (item[member].Str() == id) return &item;
    return nullptr;
}
// The adjustment `key` among an instance's parts, or nothing when the parts do not offer it.
const json::Value *Offered(const json::Value &instance, const std::wstring &key) {
    for (const json::Value &part : instance[L"parts"].Items())
        if (const json::Value *a = ById(part[L"adjustments"], L"key", key)) return a;
    return nullptr;
}

}  // namespace

// The clock's backdrop switch is a switch with a slider for how strong the backdrop is, which shows
// while the switch is on and starts at the switch's own value; the backdrop's opacity is not offered
// again among the parts. Turning the switch clears the slider's value, and undo brings it back.
TEST(AppLibraryFoldsAStrengthIntoItsSwitch) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    json::Value v = Opened(l);
    const json::Value *shade = ById(v[L"instances"][0][L"settings"], L"id", L"shade");
    CHECK(shade && (*shade)[L"value"].Str() == L"on");
    const json::Value &strength = (*shade)[L"strength"];
    CHECK(strength[L"key"].Str() == L"p1.b.opacity" && strength[L"when"].Str() == L"on");
    CHECK(strength[L"label"].Str() == L"浓度" && strength[L"control"].Str() == L"slider");
    CHECK(app::SameValue(strength[L"value"].Str(), L"0.22") && !strength[L"modified"].Bool());
    CHECK(strength[L"min"].Num() == 0 && strength[L"max"].Num() == 1);
    CHECK(!Offered(v[L"instances"][0], L"p1.b.opacity") && Offered(v[L"instances"][0], L"p1.b.color"));

    CHECK(l.SetValue(L"clock", L"p1.b.opacity", L"0.5", 3));
    v = Opened(l);
    const json::Value &moved = (*ById(v[L"instances"][0][L"settings"], L"id", L"shade"))[L"strength"];
    CHECK(app::SameValue(moved[L"value"].Str(), L"0.5") && moved[L"modified"].Bool());
    CHECK(!Offered(v[L"instances"][0], L"p1.b.opacity"));
    // Back to the switch's own value is no override.
    CHECK(l.SetValue(L"clock", L"p1.b.opacity", L"0.22", 3));
    CHECK(Own(l, kThemeT, L"clock").empty());
    CHECK(l.SetValue(L"clock", L"p1.b.opacity", L"0.6", 4));

    CHECK(l.SetValue(L"clock", L"shade", L"off"));
    CHECK(!Own(l, kThemeT, L"clock").count(L"p1.b.opacity") && Own(l, kThemeT, L"clock").at(L"shade") == L"off");
    CHECK(app::SameValue((*ById(Opened(l)[L"instances"][0][L"settings"], L"id", L"shade"))[L"strength"][L"value"].Str(), L"0"));
    CHECK(l.Undo());
    CHECK(Own(l, kThemeT, L"clock").at(L"p1.b.opacity") == L"0.6" && !Own(l, kThemeT, L"clock").count(L"shade"));
    // Putting the switch's row back puts back both.
    CHECK(l.SetValue(L"clock", L"shade", L"off") && l.SetValue(L"clock", L"p1.b.opacity", L"0.4"));
    CHECK(Own(l, kThemeT, L"clock").size() == 2);
    CHECK(l.ClearValues(L"clock", {L"shade", L"p1.b.opacity"}));
    CHECK(Own(l, kThemeT, L"clock").empty());
}

// A component of someone's own: only a strength switch's value and what a switch shows or hides
// leave the parts. A switch that sets the weight of several texts, or one value to two levels
// neither of which is 0, leaves them where they are; turning the switch clears them.
TEST(AppLibraryKeepsWhatSwitchesFeedAmongTheParts) {
    const std::wstring kOwn = L"0c0c0c0c0c0c0c0c";
    FakeStores s = Stores();
    s.store.components[kOwn] =
        "<component format=\"1\" name=\"Own\"><settings>"
        "<toggle id=\"bold\" label=\"B\" default=\"on\" on=\"700\" off=\"400\"/>"
        "<toggle id=\"dim\" label=\"D\" default=\"on\" on=\"0.5\" off=\"0\"/>"
        "<toggle id=\"fade\" label=\"F\" default=\"on\" on=\"1\" off=\"0.5\"/>"
        "<toggle id=\"date\" label=\"E\" default=\"on\" on=\"true\" off=\"false\"/>"
        "</settings><panel anchor=\"top-right\" size=\"4\">"
        "<line><text value=\"{time}\" weight=\"$bold\" opacity=\"$fade\"/>"
        "<text value=\"{date}\" weight=\"$bold\" show=\"$date\"/></line>"
        "<shadow opacity=\"$dim\"/></panel></component>";
    app::Library l(s);
    l.Open(kThemeT);
    const std::wstring id = l.AddInstance(kOwn);
    CHECK(!id.empty());
    json::Value v = Opened(l);
    const json::Value *own = ById(v[L"instances"], L"id", id);
    CHECK(own != nullptr);
    CHECK(Offered(*own, L"p1.l1.t1.weight") && Offered(*own, L"p1.l1.t2.weight"));
    CHECK(Offered(*own, L"p1.l1.t1.opacity"));
    CHECK(!Offered(*own, L"p1.l1.t2.show") && !Offered(*own, L"p1.s1.opacity") && Offered(*own, L"p1.s1.blur"));
    const json::Value &settings = (*own)[L"settings"];
    const json::Value &dim = (*ById(settings, L"id", L"dim"))[L"strength"];
    CHECK(dim[L"key"].Str() == L"p1.s1.opacity" && dim[L"label"].Str() == L"浓度" && dim[L"when"].Str() == L"on");
    for (const wchar_t *plain : {L"bold", L"fade", L"date"}) CHECK((*ById(settings, L"id", plain))[L"strength"].IsNull());
    CHECK((*ById(settings, L"id", L"date"))[L"content"].Bool());

    CHECK(l.SetValue(id, L"p1.l1.t2.weight", L"300"));
    CHECK(l.SetValue(id, L"bold", L"off"));
    CHECK(!Own(l, kThemeT, id).count(L"p1.l1.t2.weight"));
    // Shown by hand against its switch, a part stays among the parts until it is put back.
    CHECK(l.SetValue(id, L"p1.l1.t2.show", L"false"));
    CHECK(Offered(*ById(Opened(l)[L"instances"], L"id", id), L"p1.l1.t2.show"));
}

// Changing the component's colour does not mark each text's colour as changed: putting a text's
// colour back would leave it as it is. Changing where the panel is placed does mark the panel's
// position, which puts that setting back too.
TEST(AppLibraryMarksChangedOnlyWhatCanBePutBack) {
    FakeStores s = Stores();
    app::Library l(s);
    l.Open(kThemeT);
    CHECK(l.SetValue(L"clock", L"color", L"#FFD27A"));
    json::Value v = Opened(l);
    const json::Value &clock = v[L"instances"][0];
    CHECK(Offered(clock, L"p1.l1.t1.color") && !(*Offered(clock, L"p1.l1.t1.color"))[L"modified"].Bool());
    CHECK((*Offered(clock, L"p1.l1.t1.color"))[L"value"].Str() == L"#FFD27A");
    for (const json::Value &part : clock[L"parts"].Items()) CHECK(!part[L"modified"].Bool());
    CHECK((*ById(clock[L"settings"], L"id", L"color"))[L"modified"].Bool());

    CHECK(l.SetValue(L"clock", L"position", L"top-left"));
    v = Opened(l);
    CHECK((*Offered(v[L"instances"][0], L"p1.anchor"))[L"modified"].Bool());
    CHECK(l.ClearValues(L"clock", {L"p1.anchor"}));
    CHECK(Own(l, kThemeT, L"clock").size() == 1 && Own(l, kThemeT, L"clock").count(L"color"));
}

// A setting that feeds one attribute and nothing else is all there is to that value: the spectrum's
// width, style, direction, bands, colours, gradient, flow, sensitivity and smoothing are offered with
// their settings and not again on the spectrum's part page. The setting shows the value in effect,
// takes what either of them takes (the part's wider ranges), writes the value itself where only it
// takes what is asked or where the theme sets it, and its reset puts back both. Placing settings and
// switches are as before.
TEST(AppLibraryOffersASettingInPlaceOfTheOneValueItFeeds) {
    const std::wstring kSpectrum = L"0c7c0c7c0c7c0c7c";
    FakeStores s = Stores();
    skin::Skin builtIn;
    CHECK(BuiltInComponent(L"spectrum", &builtIn));
    s.store.components[kSpectrum] = skin::Normalize(builtIn);
    s.store.themes[kThemeU] = "<theme format=\"1\" name=\"U\"><wallpaper ref=\"2222222222222222\" fit=\"fill\"/>"
                              "<component id=\"bars\" ref=\"0c7c0c7c0c7c0c7c\"><set key=\"p1.l1.a1.bands\" value=\"32\"/></component></theme>";
    app::Library l(s);
    l.Open(kThemeU);
    json::Value v = Opened(l);
    const json::Value *bars = ById(v[L"instances"], L"id", L"bars");
    CHECK(bars != nullptr);
    if (!bars) return;
    const json::Value &settings = (*bars)[L"settings"];
    const std::pair<const wchar_t *, const wchar_t *> standFor[] = {
        {L"width", L"p1.l1.a1.length"}, {L"mode", L"p1.l1.a1.mode"},   {L"direction", L"p1.l1.a1.direction"},
        {L"bands", L"p1.l1.a1.bands"},  {L"colors", L"p1.l1.a1.colors"}, {L"gradient", L"p1.l1.a1.gradient"},
        {L"flow", L"p1.l1.a1.flow"},    {L"sensitivity", L"p1.l1.a1.sensitivity"}, {L"smoothing", L"p1.l1.a1.smoothing"}};
    for (const auto &[id, key] : standFor) {
        const json::Value *setting = ById(settings, L"id", id);
        CHECK(setting && (*setting)[L"adjusts"].Str() == key && !Offered(*bars, key));
    }
    // The spectrum's part keeps what no setting stands for.
    CHECK(Offered(*bars, L"p1.l1.a1.gap") && Offered(*bars, L"p1.l1.a1.height"));
    // Placing and switches as before.
    CHECK((*ById(settings, L"id", L"size"))[L"folded"].Bool() && (*ById(settings, L"id", L"size"))[L"adjusts"].IsNull());
    CHECK(!(*ById(settings, L"id", L"hide"))[L"strength"].IsNull() && (*ById(settings, L"id", L"hide"))[L"adjusts"].IsNull());
    // The wider of the two ranges, so that nothing the part offered goes out of reach.
    const json::Value &width = *ById(settings, L"id", L"width");
    CHECK(width[L"min"].Num() == 1 && width[L"max"].Num() == 80 && width[L"value"].Str() == L"13" && !width[L"modified"].Bool());
    // The theme's own value is the one in effect, and writing goes to it, which wins over the setting.
    const json::Value &bands = *ById(settings, L"id", L"bands");
    CHECK(bands[L"min"].Num() == 4 && bands[L"value"].Str() == L"32" && !bands[L"modified"].Bool());
    CHECK(l.SetValue(L"bars", L"bands", L"64"));
    CHECK(Set(l, kThemeU, L"bars", L"p1.l1.a1.bands") == L"64" && !Own(l, kThemeU, L"bars").count(L"bands"));
    CHECK((*ById((*ById(Opened(l)[L"instances"], L"id", L"bars"))[L"settings"], L"id", L"bands"))[L"value"].Str() == L"64");

    // Set on the part before (or by an older version), the value in effect shows with the setting.
    CHECK(l.SetValue(L"bars", L"p1.l1.a1.length", L"20"));
    const json::Value shown = *ById((*ById(Opened(l)[L"instances"], L"id", L"bars"))[L"settings"], L"id", L"width");
    CHECK(shown[L"value"].Str() == L"20" && shown[L"modified"].Bool());
    // Writing the setting puts the part's value back.
    CHECK(l.SetValue(L"bars", L"width", L"15"));
    CHECK(Set(l, kThemeU, L"bars", L"width") == L"15" && !Own(l, kThemeU, L"bars").count(L"p1.l1.a1.length"));
    // A width only the part takes goes to the part.
    CHECK(l.SetValue(L"bars", L"width", L"1.5"));
    CHECK(Set(l, kThemeU, L"bars", L"p1.l1.a1.length") == L"1.5" && Set(l, kThemeU, L"bars", L"width") == L"15");
    CHECK((*ById((*ById(Opened(l)[L"instances"], L"id", L"bars"))[L"settings"], L"id", L"width"))[L"value"].Str() == L"1.5");
    CHECK(!l.SetValue(L"bars", L"width", L"abc"));  // neither takes it
    // The setting's reset puts back both.
    CHECK(l.ClearValues(L"bars", {L"width", L"p1.l1.a1.length"}));
    CHECK(!Own(l, kThemeU, L"bars").count(L"width") && !Own(l, kThemeU, L"bars").count(L"p1.l1.a1.length"));

    // Someone's own component: the same rule. A colour that feeds two texts stays as it was, and so
    // do those texts' colours on their pages.
    const std::wstring kOwn = L"0d0d0d0d0d0d0d0d";
    s.store.components[kOwn] =
        "<component format=\"1\" name=\"Own\"><settings>"
        "<color id=\"tint\" label=\"T\" default=\"#FFFFFF\"/>"
        "<number id=\"fade\" label=\"F\" default=\"0.6\" min=\"0.2\" max=\"1\" step=\"0.1\"/>"
        "</settings><panel anchor=\"top\" size=\"4\">"
        "<line><text value=\"{time}\" color=\"$tint\"/><text value=\"{date}\" color=\"$tint\" opacity=\"$fade\"/></line>"
        "</panel></component>";
    app::Library own(s);
    own.Open(kThemeT);
    const std::wstring id = own.AddInstance(kOwn);
    CHECK(!id.empty());
    v = Opened(own);
    const json::Value *mine = ById(v[L"instances"], L"id", id);
    CHECK(mine != nullptr);
    if (!mine) return;
    const json::Value &fade = *ById((*mine)[L"settings"], L"id", L"fade");
    CHECK(fade[L"adjusts"].Str() == L"p1.l1.t2.opacity" && !Offered(*mine, L"p1.l1.t2.opacity") && fade[L"min"].Num() == 0);
    CHECK((*ById((*mine)[L"settings"], L"id", L"tint"))[L"adjusts"].IsNull());
    CHECK(Offered(*mine, L"p1.l1.t1.color") && Offered(*mine, L"p1.l1.t2.color"));
    CHECK(own.SetValue(id, L"fade", L"0.1") && Set(own, kThemeT, id, L"p1.l1.t2.opacity") == L"0.1");
}

TEST(AppLibraryRemembersTheRecentThemes) {
    FakeStores s = Stores();
    app::Library l(s);
    using List = std::vector<std::wstring>;
    // A settings.ini from before the list: the theme in use is offered all the same, and stays
    // on the list when another takes its place.
    CHECK(l.Recent(5) == List{kThemeT});
    CHECK(l.Apply(kThemeU, L"") && l.settings().recent == (List{kThemeU, kThemeT}));
    CHECK(l.Apply(kDefaultTheme, L"") && l.Apply(kThemeU, L""));
    CHECK(l.settings().recent == (List{kThemeU, kDefaultTheme, kThemeT}));
    CHECK(l.Recent(2) == (List{kThemeU, kDefaultTheme}) && l.Recent(10).size() == 3);
    // Renamed, a theme keeps its place; removed, it leaves the list.
    l.Move(kThemeT, L"dddddddddddddddd");
    CHECK(l.settings().recent.back() == L"dddddddddddddddd");
    CHECK(l.Recent(10) == (List{kThemeU, kDefaultTheme}));  // not installed under that id here
    l.Forget(L"dddddddddddddddd");
    CHECK(l.settings().recent == (List{kThemeU, kDefaultTheme}));
    CHECK(l.Save() && s.written.find(L"\r\nrecent = " + kThemeU + L",default\r\n") != std::wstring::npos);
    // Choosing a theme for one display in per-monitor mode counts too.
    CHECK(l.SetGlobal(L"monitorMode", L"per-monitor"));
    CHECK(l.SetGlobal(L"screen.00000000000000aa", kThemeT) && l.settings().recent.front() == kThemeT);
    // The menu's size, and whether it is shown at all.
    CHECK(l.SetGlobal(L"trayRecent", L"3") && l.settings().trayRecent == 3);
    CHECK(!l.SetGlobal(L"trayRecent", L"0") && !l.SetGlobal(L"trayRecent", L"11") && !l.SetGlobal(L"trayRecent", L"2.5"));
    CHECK(l.SetGlobal(L"tray", L"false") && !l.settings().tray);
}

TEST(AppTrayMenuMarksWhatTheLogonScreenShows) {
    FakeStores s = Stores();
    s.settings = L"theme = " + kThemeT + L"\r\ntray_recent = 2\r\nrecent = " + kThemeU + L",default," + kThemeT + L"\r\n";
    app::Library one(s);
    MonitorInfo a, b;
    a.key = L"00000000000000aa";
    a.primary = true;
    a.rect = {0, 0, 1920, 1080};
    b.key = L"00000000000000bb";
    b.rect = {1920, 0, 3200, 1024};
    // The theme in use is offered even when it is not among the newest.
    app::TrayMenu m = app::MenuFor(one, {a, b});
    CHECK(m.displays.empty() && m.themes.size() == 2);
    CHECK(m.themes[0].id == kThemeT && m.themes[0].name == L"T" && m.themes[0].checked && m.themes[0].monitor.empty());
    CHECK(m.themes[1].id == kThemeU && !m.themes[1].checked);
    CHECK(app::ShowingNow(one, {a, b}) == L"T");

    // Per-monitor mode: a submenu for each display, each marking its own theme.
    s.settings += L"monitor_mode = per-monitor\r\nscreen.00000000000000aa = " + kThemeU + L"\r\n";
    app::Library each(s);
    m = app::MenuFor(each, {a, b});
    CHECK(m.themes.empty() && m.displays.size() == 2);
    CHECK(m.displays[0].label == L"显示器 1（主显示器）" && m.displays[1].label == L"显示器 2（主显示器右侧）");
    CHECK(m.displays[0].themes.size() == 2 && m.displays[0].themes[0].id == kThemeU && m.displays[0].themes[0].checked &&
          m.displays[0].themes[0].monitor == a.key);
    CHECK(m.displays[1].themes[1].id == kThemeT && m.displays[1].themes[1].checked && m.displays[1].themes[1].monitor == b.key);
    CHECK(app::ShowingNow(each, {a, b}) == L"每个屏幕单独设置主题");
    // With one display left, one list, and the choice goes on that display.
    m = app::MenuFor(each, {a});
    CHECK(m.displays.empty() && m.themes[0].id == kThemeU && m.themes[0].checked && m.themes[0].monitor == a.key);
    CHECK(app::ShowingNow(each, {a}) == L"U");
}

// Displays are told apart by the name Windows has for them, and by where each is beside the main one,
// which tells two of the same model apart.
TEST(AppTellsDisplaysApartByNameAndPlace) {
    auto display = [](const wchar_t *key, RECT rect, bool primary, const wchar_t *name = L"", const wchar_t *device = L"") {
        MonitorInfo m;
        m.key = key;
        m.rect = rect;
        m.primary = primary;
        m.name = name;
        m.gdiName = device;
        return m;
    };
    const std::vector<MonitorInfo> all{
        display(L"main", {0, 0, 1920, 1080}, true, L"DELL U2720Q", L"\\\\.\\DISPLAY2"),
        display(L"left", {-1280, 56, 0, 1080}, false, L"DELL U2720Q", L"\\\\.\\DISPLAY1"),
        display(L"above", {0, -1080, 1920, 0}, false, L"", L"\\\\.\\DISPLAY3"),
        display(L"below", {200, 1080, 1800, 2000}, false),
        // Off the corner, as far across as down in the main display's widths and heights: beside it.
        display(L"corner", {1920, 1080, 3840, 2160}, false),
        // Off the corner but further down than across: below it.
        display(L"low", {1000, 1200, 2920, 2280}, false)};
    const app::Place places[] = {app::Place::Main, app::Place::Left, app::Place::Above, app::Place::Below, app::Place::Right, app::Place::Below};
    for (size_t i = 0; i < all.size(); ++i) CHECK(app::PlaceOf(all[i], all) == places[i]);
    CHECK(std::wstring(app::PlaceName(app::Place::Left)) == L"left" && std::wstring(app::PlaceName(app::Place::Main)) == L"main");
    // Its own name; else Windows' number for it; else its place in the list.
    CHECK(app::DisplayName(all[0], 0) == L"DELL U2720Q");
    CHECK(app::DisplayName(all[2], 2) == L"显示器 3" && app::DisplayName(all[3], 3) == L"显示器 4");
    CHECK(app::DisplayLabel(all[1], all, 1) == L"DELL U2720Q（主显示器左侧）");
    // With none marked as the main one, the first is.
    std::vector<MonitorInfo> unmarked = all;
    for (MonitorInfo &m : unmarked) m.primary = false;
    CHECK(app::PlaceOf(unmarked[0], unmarked) == app::Place::Main && app::PlaceOf(unmarked[1], unmarked) == app::Place::Left);
}
