// What the settings app's elevated helper does to the administrators-only stores in
// %ProgramData%\AnimeLogon: wallpapers\, components\ and themes\ (wallpaper.h, components.h,
// theme.h). The unelevated window normalises what the person brought in and stages it in an
// import directory; every command here reads that staging again as if it were hostile:
//   - each source is opened once, refusing links, and read only through that handle, while
//     no one can write it;
//   - metadata is parsed and written afresh, never copied; a component or theme is written in
//     its normalised form;
//   - payloads are checked for the one shape the logon screen reads: an MP4, canonical WAV
//     (wallpaper.h), a canonical bitmap of the size wallpaper.ini declares (bitmap.h);
//   - a wallpaper's sha256 is computed here from its payload, never taken from the staging;
//   - an id is never reused: a command that would write over an existing entry fails;
//   - on any failure everything the command wrote is removed again.
//
// What a staged directory holds, and nothing else (no links, no other names):
//   --commit-wallpaper  wallpaper.ini, then video.mp4 and, when wallpaper.ini says it has
//                       sound, audio.wav; or image.bmp.
//   --commit-component  component.xml
//   --commit-theme      theme.xml in installed form; each ref names a built-in or an entry
//                       already in the stores.
//   --commit-package    theme.xml in package form (theme.h);
//                       wallpaper\, staged as for --commit-wallpaper, exactly when theme.xml
//                       names a wallpaper file, and of the kind its name says (wallpaper.mp4 a
//                       video; .png, .jpg or .webp an image);
//                       components\<name>.xml for each components/<name>.xml theme.xml names.
//
// Everything is written through a Target, so tests can run the same code against a plain
// directory without elevation.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace store {

// Where the commands write. Disk() is %ProgramData%\AnimeLogon through secure.h: directories
// and files administrators-only, and the stores trusted only when secure::IsTrusted says so.
class Target {
public:
    virtual ~Target() = default;
    // The data directory; the stores are its wallpapers\, components\ and themes\.
    virtual std::wstring Root() const = 0;
    // Whether the data directory may be written into at all.
    virtual bool Ready(std::wstring *why) const = 0;
    // Whether a file already in the stores may be believed.
    virtual bool Trusted(const std::wstring &file, std::wstring *why) const = 0;
    // Creates `dir` if it is missing (and, on disk, makes it administrators-only).
    virtual DWORD MakeDirectory(const std::wstring &dir) const = 0;
    virtual DWORD WriteBytes(const std::wstring &file, const void *bytes, size_t size) const = 0;
    // Copies the rest of `source`, from its current position, into a new `file`.
    virtual DWORD CopyInto(HANDLE source, const std::wstring &file) const = 0;
    virtual DWORD RemoveTree(const std::wstring &dir) const = 0;
};
const Target &Disk();

// `dataPath`, a path under paths::DataDir() such as WallpaperDir(id), moved under t.Root().
std::wstring In(const Target &t, const std::wstring &dataPath);

// The settings app's work directory, %LOCALAPPDATA%\AnimeLogon\import\<12 hex digits>, on a
// fixed drive. The elevated side may run as another account, so it checks the shape rather
// than the profile.
bool IsImportDir(const std::wstring &dir);

// Each returns a commit::Code: kOk, kFailed, or kBadArgs for an id or directory of the wrong
// shape. Imported ids are 16 lowercase hex digits; built-ins ("default", "none", "clock")
// cannot be written or removed.
int CommitWallpaper(const Target &t, const std::wstring &id, const std::wstring &dir);
int RemoveWallpaper(const Target &t, const std::wstring &id);
int CommitComponent(const Target &t, const std::wstring &id, const std::wstring &dir);
int RemoveComponent(const Target &t, const std::wstring &id);
int CommitTheme(const Target &t, const std::wstring &id, const std::wstring &dir);
int RemoveTheme(const Target &t, const std::wstring &id);

// What --commit-package did.
struct PackageResult {
    std::wstring themeId;                              // always new
    std::wstring wallpaperId;                          // empty when the theme names a built-in
    bool wallpaperReused = false;                      // an installed wallpaper with the same sha256
    std::map<std::wstring, std::wstring> components;   // package file -> component id
    std::vector<std::wstring> componentsReused;        // ids that were installed already
};

// Installs a staged theme package under new ids. The wallpaper is one already installed when
// one has the same kind and sha256, and a component is one already installed when its
// normalised bytes are the same (or another file of the same package's are); everything else
// gets a new id. The theme's refs are rewritten to the ids, then the new components, the new
// wallpaper and the theme are written in that order; a failure removes all of them again.
int CommitPackage(const Target &t, const std::wstring &dir, PackageResult *result = nullptr);

}  // namespace store
