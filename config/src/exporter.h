// Exporting a theme as an .altheme (package.h), as the signed-in user: the stores are readable
// by Users, so no elevation is needed.
//
// What is packed is the theme as the person sees it: settings.ini's overrides for it baked in
// (theme::Edited), with any value its components do not take left out. Its wallpaper goes in as
// a file unless it is built in: a video wallpaper as its video.mp4 (the package format has no
// place for the separate sound), an image wallpaper as a PNG of its pixels. Each imported
// component goes in as components/<its id>.xml; the built-in clock as no file. No preview.png.
// The same theme with the same overrides always gives the same bytes.
#pragma once

#include <string>
#include <vector>

#include "animelogon/package.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"

namespace exporter {

// The archive's items for theme `themeId`. `error` is one sentence for the person.
bool Build(const std::wstring &themeId, const animelogon::Settings &settings, const animelogon::ThemeStore &store,
           std::vector<animelogon::package::Item> *items, std::wstring *error);

// Build from the stores on disk, written to `path`.
bool Export(const std::wstring &themeId, const animelogon::Settings &settings, const std::wstring &path,
            std::wstring *error);

}  // namespace exporter
