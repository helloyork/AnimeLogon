#include "animelogon/theme.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "animelogon/components.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"
#include "animelogon/xml.h"

namespace animelogon::theme {
namespace {

constexpr size_t kMaxLabel = 40;
constexpr size_t kMaxPackageName = 32;
const wchar_t *const kPackageWallpapers[] = {L"wallpaper.mp4", L"wallpaper.png", L"wallpaper.jpg", L"wallpaper.webp"};
constexpr wchar_t kComponentsPrefix[] = L"components/";
constexpr wchar_t kXmlSuffix[] = L".xml";

// Room for the root, the wallpaper, and every instance with all of its sets.
xml::Limits ThemeLimits() {
    xml::Limits limits;
    limits.bytes = kMaxBytes;
    limits.depth = 3;
    limits.elements = 2 + kMaxInstances * (1 + kMaxSets);
    return limits;
}

bool IsLabel(const std::wstring &text) {
    if (text.empty() || text.size() > kMaxLabel) return false;
    for (wchar_t c : text)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool IsWallpaperRefIn(Form form, const std::wstring &ref) {
    if (ref == kDefaultWallpaper || ref == kNoWallpaper) return true;
    return form == Form::Installed ? IsWallpaperId(ref) : IsPackageWallpaperRef(ref);
}

bool IsComponentRefIn(Form form, const std::wstring &ref) {
    return form == Form::Installed ? IsComponentId(ref) : ref == kClockComponent || IsPackageComponentRef(ref);
}

class Checker {
public:
    Checker(Form form, Theme *theme) : form_(form), theme_(theme) {}

    bool Run(const xml::Element &root) {
        if (root.name != L"theme") return Fail(root, L"the root element must be <theme>");
        bool format = false;
        for (const auto &[name, value] : root.attributes) {
            if (name == L"format") {
                if (value != L"1") {
                    long long n = 0;
                    newer_ = ParseInt(value, &n) && n > 1;
                    return Fail(root, newer_ ? L"format " + value + L" needs a newer AnimeLogon"
                                             : L"unsupported format " + value);
                }
                format = true;
            } else if (name == L"name") {
                if (!IsLabel(value)) return Fail(root, L"a bad name");
                theme_->name = value;
            } else if (name == L"author") {
                if (!value.empty() && !IsLabel(value)) return Fail(root, L"a bad author");
                theme_->author = value;
            } else {
                return Fail(root, L"unknown attribute " + name + L" on <theme>");
            }
        }
        if (!format || theme_->name.empty()) return Fail(root, L"<theme> needs format and name");
        bool wallpaper = false;
        for (const xml::Element &e : root.children) {
            if (e.name == L"wallpaper") {
                if (wallpaper) return Fail(e, L"<wallpaper> must come once");
                wallpaper = true;
                if (!Wallpaper(e)) return false;
            } else if (e.name == L"component") {
                if (theme_->components.size() >= kMaxInstances) return Fail(e, L"too many components");
                if (!Component(e)) return false;
            } else {
                return Fail(e, L"unknown element <" + e.name + L">");
            }
        }
        if (!wallpaper) return Fail(root, L"a theme needs a <wallpaper>");
        return true;
    }

    const std::wstring &error() const { return error_; }
    bool newer() const { return newer_; }

private:
    bool Fail(const xml::Element &e, const std::wstring &what) {
        if (error_.empty()) error_ = Format(L"line %d: %s", e.line, what.c_str());
        return false;
    }

    bool Wallpaper(const xml::Element &e) {
        if (!e.children.empty()) return Fail(e, L"<wallpaper> holds nothing");
        const std::wstring *ref = nullptr, *fit = nullptr;
        for (const auto &[name, value] : e.attributes) {
            if (name == L"ref") ref = &value;
            else if (name == L"fit") fit = &value;
            else return Fail(e, L"unknown attribute " + name + L" on <wallpaper>");
        }
        if (!ref || !fit) return Fail(e, L"<wallpaper> needs ref and fit");
        if (!IsWallpaperRefIn(form_, *ref)) return Fail(e, L"a bad wallpaper ref " + *ref);
        if (!animelogon::Parse(*fit, &theme_->fit)) return Fail(e, L"a bad fit " + *fit);
        theme_->wallpaper = *ref;
        return true;
    }

    bool Component(const xml::Element &e) {
        Instance instance;
        bool hasId = false, hasRef = false;
        for (const auto &[name, value] : e.attributes) {
            if (name == L"id") {
                instance.id = value;
                hasId = true;
            } else if (name == L"ref") {
                instance.ref = value;
                hasRef = true;
            } else if (name == L"visible") {
                if (value != L"true" && value != L"false") return Fail(e, L"visible must be true or false");
                instance.visible = value == L"true";
            } else {
                return Fail(e, L"unknown attribute " + name + L" on <component>");
            }
        }
        if (!hasId || !hasRef) return Fail(e, L"<component> needs id and ref");
        if (!IsInstanceId(instance.id)) return Fail(e, L"a bad component id " + instance.id);
        if (theme_->Find(instance.id)) return Fail(e, L"component id " + instance.id + L" is repeated");
        if (!IsComponentRefIn(form_, instance.ref)) return Fail(e, L"a bad component ref " + instance.ref);
        for (const xml::Element &s : e.children) {
            if (s.name != L"set") return Fail(s, L"a component holds only <set>");
            if (!s.children.empty()) return Fail(s, L"<set> holds nothing");
            if (instance.sets.size() >= kMaxSets) return Fail(s, L"too many sets in component " + instance.id);
            const std::wstring *key = s.Find(L"key"), *value = s.Find(L"value");
            if (!key || !value || s.attributes.size() != 2) return Fail(s, L"<set> takes key and value, and nothing else");
            if (!IsSetKey(*key)) return Fail(s, L"a bad key " + *key);
            if (!IsTypedValueText(*value)) return Fail(s, L"a bad value for " + *key);
            if (!instance.sets.emplace(*key, *value).second) return Fail(s, L"key " + *key + L" is repeated");
        }
        theme_->components.push_back(std::move(instance));
        return true;
    }

    Form form_;
    Theme *theme_;
    bool newer_ = false;
    std::wstring error_;
};

bool Failed(std::wstring *error, const std::wstring &what) {
    if (error) *error = what;
    return false;
}

}  // namespace

const Instance *Theme::Find(const std::wstring &instanceId) const {
    for (const Instance &i : components)
        if (i.id == instanceId) return &i;
    return nullptr;
}

bool IsInstanceId(const std::wstring &id) {
    if (id.empty() || id.size() > 16 || id[0] < L'a' || id[0] > L'z') return false;
    for (wchar_t c : id)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    // theme.<id>.wallpaper and theme.<id>.fit in settings.ini are the theme's own.
    return id != L"wallpaper" && id != L"fit";
}

bool IsSetKey(const std::wstring &key) {
    // theme.<id>.<instance>.ref and .visible in settings.ini are the instance's own.
    if (key == L"ref" || key == L"visible") return false;
    return skin::IsSettingId(key) || skin::IsAdjustmentKey(key);
}

bool IsPackageWallpaperRef(const std::wstring &ref) {
    for (const wchar_t *name : kPackageWallpapers)
        if (ref == name) return true;
    return false;
}

bool IsPackageComponentRef(const std::wstring &ref) {
    const size_t prefix = wcslen(kComponentsPrefix), suffix = wcslen(kXmlSuffix);
    if (ref.size() <= prefix + suffix || ref.compare(0, prefix, kComponentsPrefix) != 0 ||
        ref.compare(ref.size() - suffix, suffix, kXmlSuffix) != 0)
        return false;
    const std::wstring name = ref.substr(prefix, ref.size() - prefix - suffix);
    if (name.size() > kMaxPackageName) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        if (!alnum && (i == 0 || (c != L'-' && c != L'_'))) return false;
    }
    return true;
}

std::vector<std::wstring> PackageFiles(const Theme &theme) {
    std::vector<std::wstring> files;
    if (IsPackageWallpaperRef(theme.wallpaper)) files.push_back(theme.wallpaper);
    for (const Instance &i : theme.components)
        if (IsPackageComponentRef(i.ref) && std::find(files.begin(), files.end(), i.ref) == files.end())
            files.push_back(i.ref);
    return files;
}

bool ToInstalled(Theme *theme, const std::wstring &wallpaperId, const std::map<std::wstring, std::wstring> &componentIds,
                 std::wstring *error) {
    Theme t = *theme;
    if (IsPackageWallpaperRef(t.wallpaper)) {
        if (!IsWallpaperId(wallpaperId)) return Failed(error, L"no wallpaper id for " + t.wallpaper);
        t.wallpaper = wallpaperId;
    } else if (!IsWallpaperRef(t.wallpaper)) {
        return Failed(error, L"a bad wallpaper ref " + t.wallpaper);
    }
    for (Instance &i : t.components) {
        if (IsPackageComponentRef(i.ref)) {
            const auto it = componentIds.find(i.ref);
            if (it == componentIds.end() || !IsComponentId(it->second)) return Failed(error, L"no component id for " + i.ref);
            i.ref = it->second;
        } else if (!IsComponentId(i.ref)) {
            return Failed(error, L"a bad component ref " + i.ref);
        }
    }
    *theme = std::move(t);
    return true;
}

bool ToPackage(Theme *theme, const std::wstring &wallpaperFile, const std::map<std::wstring, std::wstring> &componentFiles,
               std::wstring *error) {
    Theme t = *theme;
    if (IsWallpaperId(t.wallpaper)) {
        if (!IsPackageWallpaperRef(wallpaperFile)) return Failed(error, L"no package file for wallpaper " + t.wallpaper);
        t.wallpaper = wallpaperFile;
    } else if (!IsWallpaperRefIn(Form::Package, t.wallpaper)) {
        return Failed(error, L"a bad wallpaper ref " + t.wallpaper);
    }
    for (Instance &i : t.components) {
        if (i.ref == kClockComponent || IsPackageComponentRef(i.ref)) continue;
        const auto it = componentFiles.find(i.ref);
        if (!IsComponentId(i.ref) || it == componentFiles.end() || !IsPackageComponentRef(it->second))
            return Failed(error, L"no package file for component " + i.ref);
        i.ref = it->second;
    }
    *theme = std::move(t);
    return true;
}

Theme Edited(const Theme &theme, const ThemeOverrides &overrides, std::vector<std::wstring> *problems) {
    Theme e = theme;
    if (overrides.wallpaper) e.wallpaper = *overrides.wallpaper;
    if (overrides.fit) e.fit = *overrides.fit;
    for (const auto &[id, o] : overrides.instances) {
        Instance *instance = nullptr;
        for (Instance &i : e.components)
            if (i.id == id) instance = &i;
        if (instance) {
            // Another component's sets were written for the one it replaces.
            if (o.ref && *o.ref != instance->ref) {
                instance->ref = *o.ref;
                instance->sets.clear();
            }
            if (o.visible) instance->visible = *o.visible;
            for (const auto &[key, value] : o.values) instance->sets[key] = value;
        } else if (!o.ref) {
            if (problems) problems->push_back(L"instance " + id + L" is not in the theme and is given no component -- ignored");
        } else if (e.components.size() >= kMaxInstances) {
            if (problems) problems->push_back(L"instance " + id + L" would make more than 8 -- ignored");
        } else {
            e.components.push_back({id, *o.ref, o.visible.value_or(true), o.values});
        }
    }
    return e;
}

std::string Normalize(const Theme &theme) {
    xml::Element root;
    root.name = L"theme";
    root.attributes = {{L"format", L"1"}, {L"name", theme.name}};
    if (!theme.author.empty()) root.attributes.emplace_back(L"author", theme.author);
    xml::Element wallpaper;
    wallpaper.name = L"wallpaper";
    wallpaper.attributes = {{L"ref", theme.wallpaper}, {L"fit", ToString(theme.fit)}};
    root.children.push_back(std::move(wallpaper));
    for (const Instance &i : theme.components) {
        xml::Element c;
        c.name = L"component";
        c.attributes = {{L"id", i.id}, {L"ref", i.ref}};
        if (!i.visible) c.attributes.emplace_back(L"visible", L"false");
        for (const auto &[key, value] : i.sets) {
            xml::Element set;
            set.name = L"set";
            set.attributes = {{L"key", key}, {L"value", value}};
            c.children.push_back(std::move(set));
        }
        root.children.push_back(std::move(c));
    }
    return xml::Write(root);
}

bool Parse(std::string_view utf8, Form form, Theme *theme, std::wstring *error, bool *newer) {
    if (newer) *newer = false;
    xml::Element root;
    if (!xml::Parse(utf8, &root, error, ThemeLimits())) return false;
    Theme parsed;
    Checker checker(form, &parsed);
    if (!checker.Run(root)) {
        if (error) *error = checker.error();
        if (newer) *newer = checker.newer();
        return false;
    }
    // Written out again it must still fit, or it could be stored but never read back.
    if (Normalize(parsed).size() > kMaxBytes) return Failed(error, L"the theme is too large once normalised");
    *theme = std::move(parsed);
    return true;
}

std::string DefaultText() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<!-- The theme that comes with AnimeLogon: the built-in wallpaper and the clock. -->
<theme format="1" name="默认" author="AnimeLogon">
  <wallpaper ref="default" fit="fill"/>
  <component id="clock" ref="clock"/>
</theme>
)";
}

const Theme &Default() {
    static const Theme theme = [] {
        Theme t;
        Parse(DefaultText(), Form::Installed, &t, nullptr);
        return t;
    }();
    return theme;
}

}  // namespace animelogon::theme

namespace animelogon {
namespace {

bool IsHexId(const std::wstring &id) {
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

bool IsThemeId(const std::wstring &id) { return id == kDefaultTheme || IsHexId(id); }

std::wstring NewThemeId() { return RandomHex(8); }

std::wstring ThemesDir() { return paths::DataDir() + L"\\themes"; }
std::wstring ThemeDir(const std::wstring &id) { return ThemesDir() + L"\\" + id; }
std::wstring ThemeFilePath(const std::wstring &id) { return ThemeDir(id) + L"\\theme.xml"; }

bool ThemeExists(const std::wstring &id) { return id == kDefaultTheme || (IsHexId(id) && IsDirectory(ThemeDir(id))); }

bool LoadTheme(const std::wstring &id, theme::Theme *theme, std::wstring *why) {
    if (id == kDefaultTheme) {
        *theme = theme::Default();
        return true;
    }
    std::wstring reason;
    std::vector<uint8_t> bytes;
    theme::Theme parsed;
    const std::wstring path = ThemeFilePath(id);
    if (!IsHexId(id)) reason = L"not a theme id";
    else if (!secure::IsTrusted(path, &reason)) reason = L"theme.xml " + reason;
    else if (!secure::ReadFileBytes(path, &bytes, theme::kMaxBytes)) reason = L"theme.xml could not be read";
    else if (!theme::Parse(std::string_view((const char *)bytes.data(), bytes.size()), theme::Form::Installed, &parsed,
                           &reason))
        reason = L"theme.xml " + reason;
    else {
        *theme = std::move(parsed);
        return true;
    }
    if (why) *why = reason;
    return false;
}

std::vector<ThemeEntry> ListThemes() {
    std::vector<ThemeEntry> out;
    const theme::Theme &d = theme::Default();
    out.push_back({kDefaultTheme, d.name, d.author, true});
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ThemesDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring id = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !IsHexId(id))
            continue;
        theme::Theme t;
        if (LoadTheme(id, &t)) out.push_back({id, t.name, t.author, false});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

}  // namespace animelogon
