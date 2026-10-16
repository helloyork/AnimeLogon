// Themes: what one display looks like -- one wallpaper and up to 8 components placed over it.
// theme.xml declares a theme in the strict XML subset components use (xml.h):
//
//   <theme format="1" name="Seaside" author="Nomen">
//     <wallpaper ref="3f2a9c0d11e4b7a8" fit="fill"/>
//     <component id="clock" ref="clock">
//       <set key="p1.anchor" value="bottom-left"/>
//       <set key="p1.l1.t1.weight" value="600"/>
//     </component>
//   </theme>
//
// - <wallpaper ref fit>, exactly once. `fit` is fill, fit or stretch.
// - <component id ref visible?>, at most 8. `id` names the instance, [a-z][a-z0-9-]{0,15},
//   unique and neither "wallpaper" nor "fit"; one component may be placed twice.
// - <set key value>, at most 128 per instance. `key` is a setting id or an adjustment key of
//   the component (skin.h); `value` is a typed value with no free text (IsTypedValueText).
//   Whether the component takes it is checked when the theme is resolved, not here.
// - `format` must be 1. `name` (1 to 40 characters) and `author` (0 to 40) are shown only in
//   the settings app, never on the logon screen.
// - The file is at most 64 KB, and so is its normalised form; unknown elements and attributes
//   are refused.
//
// An installed theme lives in %ProgramData%\AnimeLogon\themes\<id>\theme.xml, written only by
// the settings app's elevated helper, and its refs name ids: a wallpaper id, "default" or
// "none"; a component id. In a theme package (.altheme) the refs name the package's files
// instead: wallpaper.mp4, .png, .jpg or .webp, and components/<name>.xml; built-in ids
// ("default", "none", "clock") mean the same in both. ToInstalled and ToPackage rewrite them.
//
// What the person changes is never written into theme.xml: it is kept as overrides in
// settings.ini (settings.h) and applied over the theme by Edited, and by ResolveTheme
// (resolve.h).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "animelogon/settings.h"
#include "animelogon/skin.h"

namespace animelogon::theme {

constexpr size_t kMaxBytes = 64 * 1024;
constexpr size_t kMaxInstances = 8;
constexpr size_t kMaxSets = 128;  // per instance

enum class Form { Installed, Package };

struct Instance {
    std::wstring id;    // the instance's name within the theme
    std::wstring ref;   // the component it places
    bool visible = true;
    skin::Values sets;  // by setting id or adjustment key
};

struct Theme {
    std::wstring name, author;
    std::wstring wallpaper = L"default";  // the wallpaper's ref
    Scaling fit = Scaling::Fill;
    std::vector<Instance> components;

    const Instance *Find(const std::wstring &instanceId) const;
};

// `newer` is set when the file is refused only because its format is newer than 1, so the
// settings app can say that this theme needs a newer AnimeLogon.
bool Parse(std::string_view utf8, Form form, Theme *theme, std::wstring *error, bool *newer = nullptr);
// The theme as it is stored or packed: written afresh, so nothing unchecked survives. Sets are
// written in key order. The caller makes sure the theme is one Parse accepts; a theme with
// edits applied may not be (see Edited).
std::string Normalize(const Theme &theme);

bool IsInstanceId(const std::wstring &id);
// A setting id or an adjustment key, by shape alone.
bool IsSetKey(const std::wstring &key);
bool IsPackageWallpaperRef(const std::wstring &ref);  // wallpaper.mp4, .png, .jpg, .webp
bool IsPackageComponentRef(const std::wstring &ref);  // components/<name>.xml, name [a-z0-9][a-z0-9_-]{0,31}

// The files a package-form theme names: its wallpaper file, if it names one, then each
// component file once. Built-in refs name no file.
std::vector<std::wstring> PackageFiles(const Theme &theme);
// Package form to installed form, for the importer. `wallpaperId` is what the wallpaper file
// was installed as; `componentIds` maps each component file to the id it was installed as.
// Built-in refs are kept. Fails, leaving `theme` alone, when a file has no id.
bool ToInstalled(Theme *theme, const std::wstring &wallpaperId, const std::map<std::wstring, std::wstring> &componentIds,
                 std::wstring *error);
// Installed form to package form, for export: the reverse, with `wallpaperFile` (one of the
// package wallpaper names) and `componentFiles` mapping each component id to its file.
bool ToPackage(Theme *theme, const std::wstring &wallpaperFile, const std::map<std::wstring, std::wstring> &componentFiles,
               std::wstring *error);

// The theme with settings.ini's overrides for it applied, as the settings app shows and exports
// it: the wallpaper and fit replaced; instances hidden, shown, added (an override with a `ref`
// for an instance the theme does not have) or given another component (whose sets then start
// empty); override values placed over the sets. Values are not checked against the components
// here (ResolveTheme does that), and the result may exceed kMaxSets: check it with Parse before
// exporting. Overrides that cannot apply, or instances past kMaxInstances, are reported in
// `problems` and left out.
Theme Edited(const Theme &theme, const ThemeOverrides &overrides, std::vector<std::wstring> *problems = nullptr);

// The theme that comes with AnimeLogon: the built-in wallpaper, and the clock.
const Theme &Default();
std::string DefaultText();

}  // namespace animelogon::theme

namespace animelogon {

constexpr const wchar_t *kDefaultTheme = L"default";

// "default" or 16 lowercase hex digits: what `theme`, `screen.<monitor>` and the overrides'
// `theme.<id>.` may name.
bool IsThemeId(const std::wstring &id);
std::wstring NewThemeId();

std::wstring ThemesDir();
std::wstring ThemeDir(const std::wstring &id);
std::wstring ThemeFilePath(const std::wstring &id);  // theme.xml

// Whether `id` names a theme: "default", or an entry of themes\, loadable or not.
bool ThemeExists(const std::wstring &id);
// The theme `id` names: the built-in "default", or an entry of themes\ that is an
// administrators-only file (secure::IsTrusted) and parses in installed form.
bool LoadTheme(const std::wstring &id, theme::Theme *theme, std::wstring *why = nullptr);

struct ThemeEntry {
    std::wstring id, name, author;
    bool builtIn = false;
};
// The built-in theme first, then the entries of themes\ that load.
std::vector<ThemeEntry> ListThemes();

}  // namespace animelogon
