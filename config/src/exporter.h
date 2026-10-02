// Exporting a theme as an .altheme (package.h), as the signed-in user: the stores are readable
// by Users, so no elevation is needed.
//
// What is packed is the theme as the person sees it: settings.ini's overrides for it baked in
// (theme::Edited), with any value its components do not take left out. Its wallpaper goes in as
// a file unless it is built in: a video wallpaper as an MP4, an image wallpaper as a PNG of its
// pixels. A video wallpaper without sound goes in as its video.mp4, unchanged; one with sound as
// a new MP4 holding the same H.264 track and its audio.wav encoded as AAC (remux.h), since the
// format has one wallpaper.mp4 and no separate place for the sound. Each imported component goes
// in as components/<its id>.xml; the built-in clock as no file. No preview.png.
// The same theme with the same overrides always gives the same bytes.
#pragma once

#include <string>
#include <vector>

#include "animelogon/package.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"

namespace exporter {

// The archive's items for theme `themeId`. `error` is one sentence for the person.
// A file an item is read from may be one written into `workDir`, an existing directory that the
// caller removes once the archive is written.
bool Build(const std::wstring &themeId, const animelogon::Settings &settings, const animelogon::ThemeStore &store,
           const std::wstring &workDir, std::vector<animelogon::package::Item> *items, std::wstring *error);

// A new, empty directory for Build, %TEMP%\AnimeLogon-export-<12 hex digits>; empty if it cannot
// be made.
std::wstring NewWorkDir();

// Build from the stores on disk, written to `path`.
bool Export(const std::wstring &themeId, const animelogon::Settings &settings, const std::wstring &path,
            std::wstring *error);

}  // namespace exporter
