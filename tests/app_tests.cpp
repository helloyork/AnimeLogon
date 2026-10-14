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

#include "json.h"
#include "library.h"

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

}  // namespace

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
