// Imported skins: %ProgramData%\AnimeLogon\skins\<id>\skin.xml, as the settings app normalised
// it. Like the videos, they are written only by its elevated helper.
//
// Legacy: components\ (components.h) takes the place of this store. It is still read until the
// overlay and the settings app use themes.
#pragma once

#include <string>
#include <vector>

#include "animelogon/skin.h"

namespace animelogon {

std::wstring SkinsDir();
std::wstring SkinDir(const std::wstring &id);
std::wstring SkinFilePath(const std::wstring &id);

// The skin `id` names. Anything that is not an administrators-only file that parses gives the
// default skin, and `why` says what was wrong.
skin::Skin LoadSkin(const std::wstring &id, std::wstring *why = nullptr);

struct SkinEntry {
    std::wstring id, name, author;
};
// The default skin first, then the imported ones that load.
std::vector<SkinEntry> ListSkins();

}  // namespace animelogon
