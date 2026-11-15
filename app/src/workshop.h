// Wallpaper Engine's video wallpapers on this computer, so the settings app can show them and
// import the ones the person picks. Wallpaper Engine is a Steam app: what it downloads from the
// Steam Workshop is in each Steam library's steamapps\workshop\content\431960\<item>\, what the
// person made in it is in its own projects\myprojects\<name>\, and each such folder describes
// itself in project.json. Only videos are offered; scenes, web pages and programs are Wallpaper
// Engine's own formats and are not read. Everything here only reads the person's files.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::workshop {

// Where a folder of wallpapers comes from.
enum class Source { Workshop, Mine };

struct Folder {
    std::wstring path;  // ...\content\431960 or ...\projects\myprojects
    Source source = Source::Workshop;
};

// A video wallpaper: the video in one project's directory.
struct Item {
    std::wstring id;       // "<folder index>/<directory>": what the page names it by
    size_t folder = 0;     // which of the scanned folders it is in
    std::wstring dir;      // the project's directory, in that folder
    std::wstring title;    // as Wallpaper Engine shows it, on one line
    std::wstring video;    // the video's file name, in the project's directory
    std::wstring preview;  // the preview picture's file name there, or empty
    uint64_t bytes = 0;    // the video's size
};

struct Found {
    std::vector<Folder> folders;
    std::vector<Item> videos;  // in title order
    int others = 0;            // wallpapers of other types, which cannot be imported
};

// The folders Wallpaper Engine keeps wallpapers in under each of these Steam library folders
// (steam_library.h finds them), the ones that exist, each once.
std::vector<Folder> FoldersUnder(const std::vector<std::wstring> &libraries);

// What the project.json of the directory `dir` says, when it is a video wallpaper: its title
// (the directory's name when it has none), its video and preview picture as plain file names in
// that directory. False for anything else; `project` says whether the text was a project at all.
bool ReadProject(std::string_view json, const std::wstring &dir, Item *item, bool *project);

// Every video wallpaper in `folders` whose video is there.
Found Scan(const std::vector<Folder> &folders);

// The folders on this computer, under every Steam library (steam_library::Libraries). Empty when
// Steam or Wallpaper Engine is not installed.
std::vector<Folder> Folders();

}  // namespace app::workshop
