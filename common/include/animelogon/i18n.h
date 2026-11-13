// The languages AnimeLogon's own windows are shown in, and the strings in each.
//
// Every string a person reads in the settings app, the installer or the uninstaller lives in
// res/locales/<tag>.json: one flat JSON object per language, keyed by dotted names
// ("library.title"). The settings app's page bundles the same files, so a translation is one file.
// Programs that show strings carry the files as resources (the LOCALES option of
// animelogon_add_executable); a key a language lacks reads as Simplified Chinese, the language the
// strings are written in first, and a key no file has reads as itself.
//
// Which language: the person's own choice in the settings app (HKCU\Software\AnimeLogon), else
// the one picked in the installer for the machine (HKLM\SOFTWARE\AnimeLogon), else the first of
// Windows' display languages that AnimeLogon has, else English. The lock screen itself is not
// translated here: its clock follows its own language setting, and a theme's words are its own.
#pragma once

#include <windows.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace animelogon::i18n {

enum class Lang { ZhHans, ZhHant, En, Ja };
constexpr Lang kLanguages[] = {Lang::ZhHans, Lang::ZhHant, Lang::En, Lang::Ja};

// BCP 47: zh-Hans, zh-Hant, en, ja.
const wchar_t *Tag(Lang lang);
// Each language as it calls itself: 简体中文, 繁體中文, English, 日本語.
const wchar_t *NativeName(Lang lang);
// The locale DirectWrite and the page's lang attribute pick glyphs by: zh-CN, zh-TW, en-US, ja-JP.
const wchar_t *Locale(Lang lang);
bool FromTag(std::wstring_view tag, Lang *lang);
// A Windows locale or language name (zh-TW, zh-Hant-HK, ja-JP, en-GB...) to the language
// AnimeLogon shows for it; false when AnimeLogon has none for it.
bool FromLocaleName(std::wstring_view name, Lang *lang);

// The first of the person's Windows display languages that AnimeLogon has; English otherwise.
Lang SystemLanguage();

// The stored choices, as tags; empty when there is none. The machine's is written by the
// installer, which runs as an administrator; the person's by the settings app.
std::wstring UserChoice();
DWORD SetUserChoice(const std::wstring &tag);  // empty clears it
std::wstring MachineChoice();
DWORD SetMachineChoice(const std::wstring &tag);

// The person's choice, else the machine's, else the system's.
Lang Preferred();

// Makes `lang` the language Tr reads, loading its strings from this module's resources (or from
// `json`, UTF-8, where given: for tests). Safe to call while other threads translate.
void Use(Lang lang);
void UseText(Lang lang, std::string_view json, std::string_view fallbackJson);
Lang Current();

using Args = std::initializer_list<std::pair<const wchar_t *, std::wstring>>;

// The string for `key` in the current language, with each {name} replaced by its argument.
std::wstring Tr(std::string_view key);
std::wstring Tr(std::string_view key, Args args);
// Whether the current language, or the fallback, has `key`.
bool Has(std::string_view key);

// A label written into a component or a table in Simplified Chinese (a setting's name, a part's,
// a built-in component's), in the current language: the files carry each such label under
// "label.<the label>", and numbered ones ("面板 2") under "label.面板 {n}". A label they lack -- one a
// third party wrote, say -- is shown as written.
std::wstring Label(const std::wstring &text);

// One language's file, parsed: false when it is not a flat object of strings. For tests and
// for the check that every file has the same keys.
bool ParseTable(std::string_view json, void (*each)(void *context, std::string_view key, const std::wstring &value),
                void *context);

}  // namespace animelogon::i18n
