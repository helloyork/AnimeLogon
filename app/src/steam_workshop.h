// The Steam Workshop items a person subscribes to, as the theme library shows them: each
// item's folder holds one .altheme, which the person installs like any theme package, with one
// administrator's approval for as many items as they chose. Nothing here talks to Steam: what
// Steam says comes in as the helper's answers (steam_client.h), and without Steam the items are
// found from the folders Steam downloaded them to (steam_library.h).
//
// What was installed from which item is kept per person in workshop.ini (RecordsPath): the
// settings app's own file, not a public format.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"

namespace app::steam_workshop {

// --- workshop.ini --------------------------------------------------------------------------------

// What was installed from an item: the theme it became, Steam's time of the item's last update
// when it was installed (0 when Steam was not there to say), the time the .altheme's file was last
// written then (for telling a newer download apart without Steam), and the item's title.
struct Record {
    std::wstring theme;
    std::wstring title;
    long long updated = 0;
    long long file = 0;
};
using Records = std::map<std::wstring, Record>;  // by item id

// %LOCALAPPDATA%\AnimeLogon\workshop.ini, or the sandbox's own (paths.h).
std::wstring RecordsPath();
// One [installed <item id>] section per item, with theme, updated, file and title. Sections and
// keys it does not know are passed over; an item without a well-formed theme id is left out.
Records ParseRecords(std::wstring_view text);
std::wstring FormatRecords(const Records &records);
Records LoadRecords(const std::wstring &path);
// Replaces the file whole. False when it could not be written.
bool SaveRecords(const std::wstring &path, const Records &records);

// --- an item's folder ----------------------------------------------------------------------------

enum class Contents {
    Ok,        // exactly one .altheme, a plain file
    Missing,   // the folder is not there (not downloaded, or removed by Steam)
    None,      // no .altheme in it
    Several,   // more than one .altheme
    NotPlain,  // the .altheme is a link or not a file
};

struct Folder {
    Contents contents = Contents::Missing;
    std::wstring package;      // the .altheme's full path, when Ok
    long long time = 0;        // when it was last written, seconds since 1970 (UTC)
    unsigned long long size = 0;
};

// Looks into an item's folder; only its own entries, not those of folders in it.
Folder Inspect(const std::wstring &dir);

// The page's key for what is wrong with a folder that is not Ok (res/locales).
const char *ProblemKey(Contents contents);

// --- the items -----------------------------------------------------------------------------------

struct Item {
    std::wstring id;
    std::wstring title;          // empty when Steam's servers did not say
    std::wstring preview;        // the main preview's URL on Steam's servers, or empty
    std::wstring folder;         // where its files are on this computer, or empty
    bool subscribed = false;
    bool installed = false;      // its files are on this computer
    bool needsUpdate = false;    // Steam has a newer version than the files on this computer
    bool downloading = false;    // Steam is downloading it, or is to
    long long timeUpdated = 0;   // Steam's time of its last update; 0 when unknown
    unsigned long long sizeOnDisk = 0;
    std::vector<std::wstring> descriptors;
};

// The items of a `subscribed` answer (steam/PROTOCOL.md).
std::vector<Item> FromSubscribed(const json::Value &answer);

// Without Steam: an item for each folder named by an item id in these Workshop content folders
// (steam_library::WorkshopContent). Steam removes an unsubscribed item's folder, so each is taken
// as subscribed; what Steam's servers would say about it is unknown.
std::vector<Item> FromFolders(const std::vector<std::wstring> &contentDirs);

enum class State {
    Downloading,    // being downloaded, as a new item or as an update of an installed one
    NotDownloaded,  // subscribed, with no files on this computer and no download under way
    Unreadable,     // its folder is not one .altheme (Folder::contents says why)
    NotInstalled,   // ready to install
    Installed,
    Update,         // installed, and a newer version is on this computer, ready to install
    Unsubscribed,   // installed, and no longer subscribed to: the theme stays until it is deleted
};
const wchar_t *NameOf(State state);

// What the library says about an item: `record` is what workshop.ini has for it (or null), and
// `themeInstalled` whether that record's theme is still in the library.
State StateOf(const Item &item, const Folder &folder, const Record *record, bool themeInstalled);

// Whether an item in `state` can be installed now, as a new theme or over its installed one.
inline bool Installable(State state) { return state == State::NotInstalled || state == State::Update; }

// --- replacing a theme with its update -------------------------------------------------------------

// The SHA-256 the elevated helper computes for the wallpaper staged in `dir` (store.cpp): of
// image.bmp, or of video.mp4 then audio.wav. Empty when it is neither (a scene) or cannot be read.
// The helper installs a package's wallpaper as an installed one with the same SHA-256 rather than
// as a new one, so an update can only take the old wallpaper away when this differs from it.
std::wstring StagedWallpaperHash(const std::wstring &dir);

// The component as the elevated helper compares it with installed ones: parsed and written again
// (skin::Normalize). Empty when the file cannot be read or parsed.
std::string NormalizedComponent(const std::wstring &file);

// What packages staged for --commit-package would use again of what is installed: wallpapers by
// SHA-256 and components by their normalised text, as store.cpp's CommitPackage finds them.
// `...Known` is false when a staged one could not be looked at: then any installed one might be.
struct Reuse {
    std::set<std::wstring> wallpapers;
    std::set<std::string> components;
    bool wallpapersKnown = true, componentsKnown = true;
    bool MayRemoveWallpaper(const std::wstring &sha256) const {
        return wallpapersKnown && !sha256.empty() && !wallpapers.count(sha256);
    }
    bool MayRemoveComponent(const std::string &text) const {
        return componentsKnown && !text.empty() && !components.count(text);
    }
};
Reuse StagedReuse(const std::vector<std::wstring> &dirs);

}  // namespace app::steam_workshop
