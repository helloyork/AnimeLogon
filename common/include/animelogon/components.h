// Components: what a theme places over its wallpaper. A component is a file in the skin format
// (skin.h) whose root element is <component>. An imported one lives in
// %ProgramData%\AnimeLogon\components\<id>\component.xml, normalised and written only by the
// settings app's elevated helper.
//
// Built-in components have lowercase word ids: "clock" is the clock that comes with AnimeLogon
// (skin::Default()). Imported ones have 16 lowercase hex digits.
#pragma once

#include <string>
#include <vector>

#include "animelogon/skin.h"

namespace animelogon {

constexpr const wchar_t *kClockComponent = L"clock";

// "clock" or 16 lowercase hex digits: what a theme's component may name once installed.
bool IsComponentId(const std::wstring &id);
std::wstring NewComponentId();

std::wstring ComponentsDir();
std::wstring ComponentDir(const std::wstring &id);
std::wstring ComponentFilePath(const std::wstring &id);  // component.xml

// The component `id` names: a built-in one, or an entry of components\ that is an
// administrators-only file (secure::IsTrusted) and parses. `why` says what was wrong.
bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why = nullptr);

struct ComponentEntry {
    std::wstring id, name, author;
    bool builtIn = false;
};
// The built-in components first, then the entries of components\ that load.
std::vector<ComponentEntry> ListComponents();

}  // namespace animelogon
