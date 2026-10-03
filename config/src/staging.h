// The settings app's importer, run as the signed-in user: turns what the person brings in into
// what the elevated helper installs, staged in an import directory in the layouts store.h lists.
// Every decoder of outside bytes (Media Foundation for videos, WIC for pictures, the zip and XML
// readers) runs here, never in the elevated helper or the logon screen.
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace staging {

struct Result {
    bool ok = false;
    std::wstring error;  // one sentence for the person
    std::wstring name;   // the theme's or component's name, for the message after the import
};

using Progress = std::function<void(double)>;

// A new, empty import directory, %LOCALAPPDATA%\AnimeLogon\import\<12 hex digits>; empty if it
// cannot be made.
std::wstring NewImportDir();

// Whether `path` is taken for a picture rather than a video, by its extension.
bool LooksLikePicture(const std::wstring &path);

// A video or a picture as a new theme, staged for --commit-package: the wallpaper normalised
// into dir\wallpaper\ (a video transcoded, a picture decoded to image.bmp), and a theme.xml
// named after the file that places it and the clock. Videos need COM on the calling thread.
Result Media(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
             const Progress &progress);

// An .altheme, staged for --commit-package. The archive must pass package::Reader; its
// theme.xml must parse; the wallpaper file it carries must be the one theme.xml names (or none
// when theme.xml names a built-in wallpaper or none), and its component files exactly those
// theme.xml names. Components are written normalised, the wallpaper goes through the same
// normalisation as Media, and theme.xml is written afresh.
Result Package(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel,
               const Progress &progress);

// A component file (.xml), staged for --commit-component as dir\component.xml, normalised.
Result Component(const std::wstring &source, const std::wstring &dir);

}  // namespace staging
