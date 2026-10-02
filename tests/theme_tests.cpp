// theme.xml, components and wallpaper.ini: parsing, normalising, refusing, and the built-ins.
#include "check.h"

#include <string>

#include "animelogon/components.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

using namespace animelogon;

namespace {

const std::string kWallpaper = "<wallpaper ref=\"default\" fit=\"fill\"/>";
const std::string kClock = "<component id=\"clock\" ref=\"clock\"/>";

std::string Theme(const std::string &body, const std::string &attributes = "format=\"1\" name=\"t\"") {
    return "<theme " + attributes + ">" + body + "</theme>";
}

bool Parses(const std::string &text, theme::Form form = theme::Form::Installed, std::wstring *error = nullptr) {
    theme::Theme t;
    std::wstring e;
    const bool ok = theme::Parse(text, form, &t, &e);
    if (error) *error = e;
    return ok;
}

std::string Sets(size_t count, size_t valueLength = 1, size_t first = 0) {
    std::string out;
    for (size_t i = first; i < first + count; ++i)
        out += "<set key=\"s" + std::to_string(i) + "\" value=\"" + std::string(valueLength, 'v') + "\"/>";
    return out;
}

bool ParsesComponent(const std::string &text) {
    skin::Skin s;
    return skin::Parse(text, &s, nullptr);
}

const std::string kPanel = "<panel><line><text value=\"{time}\"/></line></panel>";

}  // namespace

TEST(ThemeParsesAndNormalizes) {
    const std::string text =
        "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- a theme -->\n"
        "<theme format=\"1\" name=\"\xE5\xA4\x8F\xE6\x97\xA5\" author=\"Nomen\">\n"
        "  <component id=\"clock\" ref=\"clock\">\n"
        "    <set key=\"p1.offset-x\" value=\"4\"/>\n"
        "    <set key=\"p1.anchor\" value=\"bottom-left\"/>\n"
        "  </component>\n"
        "  <wallpaper ref=\"3f2a9c0d11e4b7a8\" fit=\"fit\"/>\n"
        "  <component id=\"clock-2\" ref=\"0123456789abcdef\" visible=\"false\"><set key=\"color\" value=\"#FFD27F\"/></component>\n"
        "</theme>\n";
    theme::Theme t;
    std::wstring error;
    CHECK(theme::Parse(text, theme::Form::Installed, &t, &error));
    CHECK(error.empty());
    CHECK(t.name == L"\x590F\x65E5" && t.author == L"Nomen");
    CHECK(t.wallpaper == L"3f2a9c0d11e4b7a8" && t.fit == Scaling::Fit);
    CHECK(t.components.size() == 2);
    const theme::Instance *clock = t.Find(L"clock");
    CHECK(clock && clock->ref == L"clock" && clock->visible && clock->sets.size() == 2);
    CHECK(clock && clock->sets.at(L"p1.anchor") == L"bottom-left");
    const theme::Instance *second = t.Find(L"clock-2");
    CHECK(second && second->ref == L"0123456789abcdef" && !second->visible && second->sets.at(L"color") == L"#FFD27F");
    CHECK(!t.Find(L"nothing"));

    // Normalised: the wallpaper first, the instances in their order, the sets by key; and stable.
    const std::string normal = theme::Normalize(t);
    CHECK(normal.find("<wallpaper") < normal.find("<component"));
    CHECK(normal.find("p1.anchor") < normal.find("p1.offset-x"));
    CHECK(normal.find("visible=\"false\"") != std::string::npos);
    theme::Theme again;
    CHECK(theme::Parse(normal, theme::Form::Installed, &again, nullptr));
    CHECK(theme::Normalize(again) == normal);
    CHECK(again.name == t.name && again.author == t.author && again.wallpaper == t.wallpaper && again.fit == t.fit);
    CHECK(again.components.size() == 2 && again.components[1].id == L"clock-2" && !again.components[1].visible);
    CHECK(again.components[0].sets == clock->sets);

    // No author is written as none.
    theme::Theme anonymous;
    CHECK(theme::Parse(Theme(kWallpaper), theme::Form::Installed, &anonymous, nullptr));
    CHECK(anonymous.author.empty() && anonymous.components.empty() && anonymous.fit == Scaling::Fill);
    CHECK(theme::Normalize(anonymous).find("author") == std::string::npos);
}

TEST(ThemeRefusesBadStructure) {
    CHECK(Parses(Theme(kWallpaper + kClock)));
    // The root and its attributes.
    CHECK(!Parses("<skin format=\"1\" name=\"t\">" + kWallpaper + "</skin>"));
    CHECK(!Parses(Theme(kWallpaper, "name=\"t\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\" name=\"\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\" name=\"" + std::string(41, 'n') + "\"")));
    CHECK(Parses(Theme(kWallpaper, "format=\"1\" name=\"" + std::string(40, 'n') + "\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\" name=\"a&#9;b\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\" name=\"t\" author=\"a&#9;b\"")));
    CHECK(!Parses(Theme(kWallpaper, "format=\"1\" name=\"t\" version=\"2\"")));
    // Elements.
    CHECK(!Parses(Theme(kWallpaper + "<image/>")));
    CHECK(!Parses(Theme(kWallpaper + "text")));
    CHECK(!Parses(Theme("")));
    CHECK(!Parses(Theme(kClock)));
    CHECK(!Parses(Theme(kWallpaper + kWallpaper)));
    // The wallpaper.
    CHECK(!Parses(Theme("<wallpaper ref=\"default\"/>")));
    CHECK(!Parses(Theme("<wallpaper fit=\"fill\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"default\" fit=\"cover\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"../x\" fit=\"fill\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"0123456789ABCDEF\" fit=\"fill\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"wallpaper.mp4\" fit=\"fill\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"default\" fit=\"fill\" loop=\"true\"/>")));
    CHECK(!Parses(Theme("<wallpaper ref=\"default\" fit=\"fill\"><set key=\"a\" value=\"b\"/></wallpaper>")));
    CHECK(Parses(Theme("<wallpaper ref=\"none\" fit=\"stretch\"/>")));
    CHECK(Parses(Theme("<wallpaper ref=\"0123456789abcdef\" fit=\"fit\"/>")));
}

TEST(ThemeRefusesBadInstances) {
    auto with = [](const std::string &components) { return Parses(Theme(kWallpaper + components)); };
    CHECK(!with("<component ref=\"clock\"/>"));
    CHECK(!with("<component id=\"clock\"/>"));
    for (const char *id : {"Clock", "1clock", "-clock", "a_b", "a.b", "", "wallpaper", "fit", "abcdefghijklmnopq"})
        CHECK(!with("<component id=\"" + std::string(id) + "\" ref=\"clock\"/>"));
    CHECK(with("<component id=\"abcdefghijklmnop\" ref=\"clock\"/>"));
    CHECK(with("<component id=\"a-1\" ref=\"clock\"/>"));
    CHECK(!with(kClock + kClock));
    CHECK(with(kClock + "<component id=\"again\" ref=\"clock\"/>"));
    // Refs: an installed component id; not the old skin name, nor a package file.
    CHECK(!with("<component id=\"c\" ref=\"default\"/>"));
    CHECK(!with("<component id=\"c\" ref=\"components/a.xml\"/>"));
    CHECK(!with("<component id=\"c\" ref=\"battery\"/>"));
    CHECK(with("<component id=\"c\" ref=\"0123456789abcdef\"/>"));
    CHECK(!with("<component id=\"c\" ref=\"clock\" visible=\"yes\"/>"));
    CHECK(with("<component id=\"c\" ref=\"clock\" visible=\"true\"/>"));
    CHECK(!with("<component id=\"c\" ref=\"clock\" x=\"1\"/>"));
    CHECK(!with("<component id=\"c\" ref=\"clock\"><panel/></component>"));
    // At most 8 instances.
    std::string eight;
    for (int i = 0; i < 8; ++i) eight += "<component id=\"c" + std::to_string(i) + "\" ref=\"clock\"/>";
    CHECK(with(eight));
    CHECK(!with(eight + "<component id=\"c8\" ref=\"clock\"/>"));
}

TEST(ThemeRefusesBadSets) {
    auto set = [](const std::string &sets) {
        return Parses(Theme(kWallpaper + "<component id=\"clock\" ref=\"clock\">" + sets + "</component>"));
    };
    CHECK(set("<set key=\"color\" value=\"#FFFFFF\"/><set key=\"p1.l1.t1.weight\" value=\"600\"/>"));
    CHECK(set("<set key=\"p1.b.opacity\" value=\"\"/>"));  // typed values are checked when resolved
    CHECK(!set("<set key=\"color\"/>"));
    CHECK(!set("<set value=\"x\"/>"));
    CHECK(!set("<set key=\"color\" value=\"#FFFFFF\" extra=\"1\"/>"));
    CHECK(!set("<set key=\"color\" value=\"#FFFFFF\"><set key=\"a\" value=\"b\"/></set>"));
    CHECK(!set("<value key=\"color\" value=\"#FFFFFF\"/>"));
    for (const char *key : {"ref", "visible", "Color", "a b", "p1.l1.t1.value", "p1.l1.t1.hang", "p0.size", "p1.q1.size", ""})
        CHECK(!set("<set key=\"" + std::string(key) + "\" value=\"1\"/>"));
    CHECK(!set("<set key=\"color\" value=\"a&#9;b\"/>"));
    CHECK(set("<set key=\"color\" value=\"" + std::string(128, 'x') + "\"/>"));
    CHECK(!set("<set key=\"color\" value=\"" + std::string(129, 'x') + "\"/>"));
    CHECK(!set("<set key=\"color\" value=\"#FFFFFF\"/><set key=\"color\" value=\"#000000\"/>"));
    // At most 128 per instance.
    CHECK(set(Sets(128)));
    CHECK(!set(Sets(129)));
}

TEST(ThemeLimits) {
    // Eight instances of 128 sets each fit the element limit.
    std::string full;
    for (int i = 0; i < 8; ++i)
        full += "<component id=\"c" + std::to_string(i) + "\" ref=\"clock\">" + Sets(128, 1) + "</component>";
    std::wstring error;
    CHECK(Parses(Theme(kWallpaper + full), theme::Form::Installed, &error));
    // The file is at most 64 KB.
    const std::string padded = Theme(kWallpaper) + "<!--" + std::string(theme::kMaxBytes, ' ') + "-->";
    CHECK(!Parses(padded));
    // And so is what it normalises to: compact sets grow by their indentation when written out.
    std::string compact;
    for (int i = 0; i < 8; ++i)
        compact += "<component id=\"c" + std::to_string(i) + "\" ref=\"clock\">" + Sets(128, 36) + "</component>";
    const std::string tight = Theme(kWallpaper + compact);
    CHECK(tight.size() <= theme::kMaxBytes);
    CHECK(!Parses(tight, theme::Form::Installed, &error));
    CHECK(error.find(L"normalised") != std::wstring::npos);
}

TEST(ThemeNewerFormat) {
    theme::Theme t;
    std::wstring error;
    bool newer = false;
    CHECK(!theme::Parse(Theme(kWallpaper + "<scene/>", "format=\"2\" name=\"t\""), theme::Form::Installed, &t, &error, &newer));
    CHECK(newer);
    CHECK(!theme::Parse(Theme(kWallpaper, "format=\"x\" name=\"t\""), theme::Form::Installed, &t, &error, &newer));
    CHECK(!newer);
    CHECK(!theme::Parse(Theme(kWallpaper, "format=\"0\" name=\"t\""), theme::Form::Installed, &t, &error, &newer));
    CHECK(!newer);
}

TEST(ThemePackageForm) {
    const std::string text = Theme(
        "<wallpaper ref=\"wallpaper.webp\" fit=\"stretch\"/>"
        "<component id=\"bat-a\" ref=\"components/battery.xml\"><set key=\"p1.anchor\" value=\"top-right\"/></component>"
        "<component id=\"bat-b\" ref=\"components/battery.xml\"/>"
        "<component id=\"clock\" ref=\"clock\"/>"
        "<component id=\"np\" ref=\"components/now_playing-2.xml\"/>");
    theme::Theme t;
    CHECK(theme::Parse(text, theme::Form::Package, &t, nullptr));
    CHECK(!Parses(text, theme::Form::Installed));
    // Installed refs are not package refs, but the built-ins are both.
    CHECK(!Parses(Theme("<wallpaper ref=\"0123456789abcdef\" fit=\"fill\"/>"), theme::Form::Package));
    CHECK(!Parses(Theme(kWallpaper + "<component id=\"c\" ref=\"0123456789abcdef\"/>"), theme::Form::Package));
    CHECK(Parses(Theme(kWallpaper + kClock), theme::Form::Package));
    CHECK(Parses(Theme("<wallpaper ref=\"none\" fit=\"fill\"/>"), theme::Form::Package));
    for (const char *name : {"wallpaper.mp4", "wallpaper.png", "wallpaper.jpg", "wallpaper.webp"})
        CHECK(theme::IsPackageWallpaperRef(FromUtf8(name)));
    for (const char *name : {"wallpaper.gif", "Wallpaper.mp4", "wallpaper.MP4", "media/wallpaper.mp4", "wallpaper"})
        CHECK(!theme::IsPackageWallpaperRef(FromUtf8(name)));
    CHECK(theme::IsPackageComponentRef(L"components/a.xml"));
    CHECK(theme::IsPackageComponentRef(L"components/" + std::wstring(32, L'a') + L".xml"));
    for (const wchar_t *name : {L"components/Battery.xml", L"components/../x.xml", L"components/.xml", L"components/a/b.xml",
                                L"components/_a.xml", L"components/-a.xml", L"components\\a.xml", L"a.xml", L"components/a.XML"})
        CHECK(!theme::IsPackageComponentRef(name));
    CHECK(!theme::IsPackageComponentRef(L"components/" + std::wstring(33, L'a') + L".xml"));

    // The files it names, each once.
    const std::vector<std::wstring> files = theme::PackageFiles(t);
    CHECK(files.size() == 3 && files[0] == L"wallpaper.webp" && files[1] == L"components/battery.xml" &&
          files[2] == L"components/now_playing-2.xml");

    // Installing rewrites the file refs to the ids they were installed as, and nothing else.
    const std::map<std::wstring, std::wstring> ids = {{L"components/battery.xml", L"0a7b0a7b0a7b0a7b"},
                                                      {L"components/now_playing-2.xml", L"1111222233334444"}};
    theme::Theme installed = t;
    std::wstring error;
    CHECK(!theme::ToInstalled(&installed, L"", ids, &error));
    CHECK(installed.wallpaper == L"wallpaper.webp");  // left alone on failure
    CHECK(!theme::ToInstalled(&installed, L"9c1d9c1d9c1d9c1d", {{L"components/battery.xml", L"0a7b0a7b0a7b0a7b"}}, &error));
    CHECK(installed.components[0].ref == L"components/battery.xml");
    CHECK(!theme::ToInstalled(&installed, L"9c1d9c1d9c1d9c1d",
                              {{L"components/battery.xml", L"../x"}, {L"components/now_playing-2.xml", L"1111222233334444"}},
                              &error));
    CHECK(theme::ToInstalled(&installed, L"9c1d9c1d9c1d9c1d", ids, &error));
    CHECK(installed.wallpaper == L"9c1d9c1d9c1d9c1d" && installed.fit == Scaling::Stretch);
    CHECK(installed.components[0].ref == L"0a7b0a7b0a7b0a7b" && installed.components[1].ref == L"0a7b0a7b0a7b0a7b");
    CHECK(installed.components[2].ref == L"clock" && installed.components[3].ref == L"1111222233334444");
    CHECK(installed.components[0].sets.at(L"p1.anchor") == L"top-right");
    CHECK(Parses(theme::Normalize(installed), theme::Form::Installed));
    CHECK(theme::PackageFiles(installed).empty());

    // Exporting goes back.
    theme::Theme packed = installed;
    CHECK(!theme::ToPackage(&packed, L"wallpaper.gif", {}, &error));
    CHECK(!theme::ToPackage(&packed, L"wallpaper.webp", {{L"0a7b0a7b0a7b0a7b", L"components/battery.xml"}}, &error));
    CHECK(packed.wallpaper == L"9c1d9c1d9c1d9c1d");
    CHECK(theme::ToPackage(&packed, L"wallpaper.webp",
                           {{L"0a7b0a7b0a7b0a7b", L"components/battery.xml"},
                            {L"1111222233334444", L"components/now_playing-2.xml"}},
                           &error));
    CHECK(theme::Normalize(packed) == theme::Normalize(t));

    // A theme on a built-in wallpaper packs without a wallpaper file.
    theme::Theme plain = theme::Default();
    CHECK(theme::ToPackage(&plain, L"", {}, &error) && plain.wallpaper == L"default");
    CHECK(theme::PackageFiles(plain).empty());
}

TEST(ThemeEditedByOverrides) {
    theme::Theme t;
    CHECK(theme::Parse(Theme("<wallpaper ref=\"0123456789abcdef\" fit=\"fit\"/>"
                             "<component id=\"clock\" ref=\"clock\"><set key=\"color\" value=\"#111111\"/>"
                             "<set key=\"size\" value=\"12\"/></component>"
                             "<component id=\"other\" ref=\"0a7b0a7b0a7b0a7b\" visible=\"false\"><set key=\"x\" value=\"1\"/></component>"),
                       theme::Form::Installed, &t, nullptr));
    ThemeOverrides o;
    o.wallpaper = L"none";
    o.fit = Scaling::Stretch;
    o.instances[L"clock"].values[L"color"] = L"#222222";
    o.instances[L"clock"].values[L"p1.l1.t1.weight"] = L"600";
    o.instances[L"other"].visible = true;
    o.instances[L"other"].ref = L"1111222233334444";
    o.instances[L"added"].ref = L"clock";
    o.instances[L"added"].values[L"size"] = L"8";
    o.instances[L"ghost"].values[L"size"] = L"8";
    std::vector<std::wstring> problems;
    const theme::Theme e = theme::Edited(t, o, &problems);
    CHECK(e.wallpaper == L"none" && e.fit == Scaling::Stretch);
    CHECK(e.components.size() == 3);
    const theme::Instance *clock = e.Find(L"clock");
    CHECK(clock && clock->sets.size() == 3 && clock->sets.at(L"color") == L"#222222" && clock->sets.at(L"size") == L"12");
    const theme::Instance *other = e.Find(L"other");
    CHECK(other && other->visible && other->ref == L"1111222233334444" && other->sets.empty());
    const theme::Instance *added = e.Find(L"added");
    CHECK(added && added->ref == L"clock" && added->visible && added->sets.at(L"size") == L"8");
    CHECK(!e.Find(L"ghost"));
    CHECK(problems.size() == 1);
    // No overrides: the theme itself.
    CHECK(theme::Normalize(theme::Edited(t, ThemeOverrides{})) == theme::Normalize(t));
    // Never more than 8 instances.
    ThemeOverrides many;
    for (int i = 0; i < 9; ++i) many.instances[L"n" + std::to_wstring(i)].ref = L"clock";
    problems.clear();
    const theme::Theme crowded = theme::Edited(t, many, &problems);
    CHECK(crowded.components.size() == 8 && problems.size() == 3);
}

TEST(ThemeDefaultIsBuiltIn) {
    const theme::Theme &d = theme::Default();
    CHECK(!d.name.empty() && d.wallpaper == L"default" && d.fit == Scaling::Fill);
    CHECK(d.components.size() == 1 && d.components[0].id == L"clock" && d.components[0].ref == L"clock");
    CHECK(d.components[0].visible && d.components[0].sets.empty());
    std::wstring error;
    theme::Theme parsed;
    CHECK(theme::Parse(theme::DefaultText(), theme::Form::Installed, &parsed, &error) && error.empty());
    CHECK(theme::Parse(theme::DefaultText(), theme::Form::Package, &parsed, &error));
    theme::Theme loaded;
    CHECK(LoadTheme(L"default", &loaded) && theme::Normalize(loaded) == theme::Normalize(d));
    CHECK(!LoadTheme(L"../x", &loaded) && !LoadTheme(L"Default", &loaded));
    CHECK(ThemeExists(L"default"));
    CHECK(IsThemeId(L"default") && IsThemeId(L"0123456789abcdef") && IsThemeId(NewThemeId()));
    CHECK(!IsThemeId(L"Default") && !IsThemeId(L"0123456789ABCDEF") && !IsThemeId(L"..\\x") && !IsThemeId(L""));
    const std::vector<ThemeEntry> themes = ListThemes();
    CHECK(!themes.empty() && themes[0].id == L"default" && themes[0].builtIn && themes[0].name == d.name);
    CHECK(ThemeFilePath(L"0123456789abcdef").size() > 40 &&
          ThemeFilePath(L"0123456789abcdef").find(L"\\themes\\0123456789abcdef\\theme.xml") != std::wstring::npos);

    // The built-in wallpaper has no files; "none" is not a wallpaper.
    WallpaperInfo w;
    CHECK(LoadWallpaper(L"default", &w) && w.builtIn && w.kind == WallpaperKind::Image && w.id == L"default");
    CHECK(w.videoPath.empty() && w.audioPath.empty() && w.imagePath.empty() && !w.legacy);
    CHECK(!LoadWallpaper(L"none", &w) && !LoadWallpaper(L"../x", &w));
    const std::vector<WallpaperInfo> wallpapers = ListWallpapers();
    CHECK(!wallpapers.empty() && wallpapers[0].builtIn);

    // The built-in component is the clock; "default" is its old name.
    skin::Skin clock;
    CHECK(LoadComponent(L"clock", &clock) && clock.name == skin::Default().name);
    CHECK(LoadComponent(L"default", &clock) && skin::Normalize(clock) == skin::Normalize(skin::Default()));
    CHECK(!LoadComponent(L"Clock", &clock) && !LoadComponent(L"..\\x", &clock));
    CHECK(IsComponentId(L"clock") && IsComponentId(L"0123456789abcdef") && IsComponentId(NewComponentId()));
    CHECK(!IsComponentId(L"default") && !IsComponentId(L"Clock") && !IsComponentId(L""));
    const std::vector<ComponentEntry> components = ListComponents();
    CHECK(!components.empty() && components[0].id == L"clock" && components[0].builtIn);
}

TEST(ComponentRootIsRenamed) {
    CHECK(skin::DefaultText().find("<component ") != std::string::npos);
    CHECK(skin::DefaultText().find("<skin") == std::string::npos);
    CHECK(ParsesComponent("<component format=\"1\" name=\"t\">" + kPanel + "</component>"));
    CHECK(ParsesComponent("<skin format=\"1\" name=\"t\">" + kPanel + "</skin>"));  // transitional
    CHECK(!ParsesComponent("<widget format=\"1\" name=\"t\">" + kPanel + "</widget>"));
    CHECK(!ParsesComponent("<component format=\"1\">" + kPanel + "</component>"));
    // Normalising writes the new name, whatever was read.
    skin::Skin old;
    CHECK(skin::Parse("<skin format=\"1\" name=\"t\">" + kPanel + "</skin>", &old, nullptr));
    const std::string normal = skin::Normalize(old);
    CHECK(normal.find("<component ") != std::string::npos && normal.find("</component>") != std::string::npos);
    CHECK(normal.find("skin") == std::string::npos);
    CHECK(ParsesComponent(normal));
    // settings.ini gives "ref" and "visible" to the instance, so no component may use them.
    for (const char *id : {"ref", "visible"})
        CHECK(!ParsesComponent("<component format=\"1\" name=\"t\"><settings><toggle id=\"" + std::string(id) +
                               "\" label=\"T\" default=\"on\" on=\"1\" off=\"0\"/></settings>" + kPanel + "</component>"));
    CHECK(ParsesComponent("<component format=\"1\" name=\"t\"><settings><toggle id=\"shown\" label=\"T\" default=\"on\" "
                          "on=\"1\" off=\"0\"/></settings>" + kPanel + "</component>"));
}

TEST(ComponentAcceptsWhatItHas) {
    const skin::Skin &s = skin::Default();
    CHECK(skin::Accepts(s, L"color", L"#12ABEF"));
    CHECK(!skin::Accepts(s, L"color", L"blue"));
    CHECK(skin::Accepts(s, L"size", L"20") && !skin::Accepts(s, L"size", L"30"));
    CHECK(skin::Accepts(s, L"shade", L"off") && !skin::Accepts(s, L"shade", L"0"));
    CHECK(!skin::Accepts(s, L"nosuch", L"1"));
    CHECK(skin::Accepts(s, L"p1.size", L"12"));
    CHECK(skin::Accepts(s, L"p1.l1.t1.weight", L"600") && skin::Accepts(s, L"p1.l1.t2.weight", L"bold"));
    CHECK(!skin::Accepts(s, L"p1.l1.t1.weight", L"heavy"));
    CHECK(!skin::Accepts(s, L"p1.l1.t3.weight", L"600"));  // the first line has two texts
    CHECK(!skin::Accepts(s, L"p1.l3.t1.weight", L"600"));  // and the panel two lines
    CHECK(skin::Accepts(s, L"p1.l2.gap", L"0.3") && skin::Accepts(s, L"p1.l1.gap", L"0.3"));
    CHECK(skin::Accepts(s, L"p1.s1.opacity", L"0.5") && !skin::Accepts(s, L"p1.s2.opacity", L"0.5"));
    CHECK(skin::Accepts(s, L"p1.b.opacity", L"0.5") && !skin::Accepts(s, L"p1.b.opacity", L"2"));
    CHECK(!skin::Accepts(s, L"p2.size", L"10"));
    CHECK(!skin::Accepts(s, L"p1.l1.t1.value", L"{date}"));
}

TEST(WallpaperInfoRoundTrip) {
    WallpaperInfo v;
    v.kind = WallpaperKind::Video;
    v.name = L"\x665A\x971E\r\nsecond line";
    v.sourceName = L"clip.mkv";
    v.importedAt = L"2026-10-02T12:00:00";
    v.width = 1920;
    v.height = 1080;
    v.frameRateNum = 30000;
    v.frameRateDen = 1001;
    v.durationMs = 12345;
    v.hasAudio = true;
    v.sha256 = std::wstring(64, L'a');
    WallpaperInfo w;
    std::wstring why;
    CHECK(ParseWallpaperInfo(SerializeWallpaperInfo(v), &w, &why));
    CHECK(w.kind == WallpaperKind::Video && w.name == L"\x665A\x971E  second line" && w.sourceName == L"clip.mkv");
    CHECK(w.importedAt == v.importedAt && w.width == 1920 && w.height == 1080);
    CHECK(w.frameRateNum == 30000 && w.frameRateDen == 1001 && w.durationMs == 12345 && w.hasAudio);
    CHECK(w.sha256 == v.sha256);
    CHECK(w.videoPath.empty() && w.bytes == 0 && !w.builtIn && !w.legacy);

    WallpaperInfo image;
    image.kind = WallpaperKind::Image;
    image.name = L"sea";
    image.width = 3840;
    image.height = 2160;
    image.sha256 = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::wstring text = SerializeWallpaperInfo(image);
    CHECK(text.find(L"type = image") != std::wstring::npos && text.find(L"frame_rate") == std::wstring::npos);
    CHECK(ParseWallpaperInfo(text, &w) && w.kind == WallpaperKind::Image && w.width == 3840 && w.height == 2160);
    CHECK(w.sha256 == image.sha256 && !w.hasAudio && w.durationMs == 0);
    // Video fields mean nothing for an image.
    CHECK(ParseWallpaperInfo(L"type = image\nwidth = 10\nheight = 10\naudio = true\n", &w) && !w.hasAudio);
    // Without a hash it still loads; unknown keys are ignored.
    CHECK(ParseWallpaperInfo(L"type = image\nwidth = 10\nheight = 10\nfuture = 1\n", &w) && w.sha256.empty());

    CHECK(!ParseWallpaperInfo(L"type = scene\nwidth = 10\nheight = 10\n", &w, &why));
    CHECK(why.find(L"scene") != std::wstring::npos);
    CHECK(!ParseWallpaperInfo(L"width = 10\nheight = 10\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = gif\nwidth = 10\nheight = 10\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = image\nwidth = 10\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = image\nwidth = 7681\nheight = 10\n", &w));
    CHECK(ParseWallpaperInfo(L"type = image\nwidth = 7680\nheight = 4320\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = video\nwidth = 10\nheight = 10\nduration_ms = 5\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = video\nwidth = 10\nheight = 10\nframe_rate = 30/1\n", &w));
    CHECK(ParseWallpaperInfo(L"type = video\nwidth = 10\nheight = 10\nframe_rate = 30/1\nduration_ms = 5\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = image\nwidth = 10\nheight = 10\nsha256 = " + std::wstring(63, L'a') + L"\n", &w));
    CHECK(!ParseWallpaperInfo(L"type = image\nwidth = 10\nheight = 10\nsha256 = " + std::wstring(64, L'A') + L"\n", &w));

    CHECK(IsWallpaperId(NewWallpaperId()) && !IsWallpaperId(L"default") && !IsWallpaperId(L"0123456789ABCDEF"));
    CHECK(IsWallpaperRef(L"default") && IsWallpaperRef(L"none") && IsWallpaperRef(L"0123456789abcdef"));
    CHECK(!IsWallpaperRef(L"wallpaper.mp4") && !IsWallpaperRef(L""));
    const std::wstring dir = L"\\wallpapers\\0123456789abcdef\\";
    CHECK(WallpaperVideoPath(L"0123456789abcdef").find(dir + L"video.mp4") != std::wstring::npos);
    CHECK(WallpaperAudioPath(L"0123456789abcdef").find(dir + L"audio.wav") != std::wstring::npos);
    CHECK(WallpaperImagePath(L"0123456789abcdef").find(dir + L"image.bmp") != std::wstring::npos);
    CHECK(WallpaperInfoPath(L"0123456789abcdef").find(dir + L"wallpaper.ini") != std::wstring::npos);
}
