// Where Steam keeps things on this computer: its own folder, the library folders it lists, and in
// each library the apps' installed files and their Steam Workshop downloads. None of this needs
// Steam to be running or loads its library; everything here only reads files Steam wrote.
//
// A library is a folder holding steamapps\: Steam's own folder is the first, the others are listed
// in its libraryfolders.vdf. In a library, an app's files are in steamapps\common\<installdir>\,
// where <installdir> is named by steamapps\appmanifest_<appid>.acf, and what the person subscribed
// to in its Workshop is in steamapps\workshop\content\<appid>\<item id>\.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::steam_library {

// The path as one spelling, to see that two name the same folder: forward slashes become
// backslashes and trailing separators go (but a drive's root keeps its own).
std::wstring Canonical(std::wstring path);

// Where Steam is installed, from the registry: the person's SteamPath, else the machine's
// InstallPath. Empty when Steam is not installed.
std::wstring SteamDir();

// The Steam library folders named in the text of Steam's libraryfolders.vdf, in either layout
// Steam has written: a "path" value in a block per library, or (before 2021) the path as the
// value of a numbered key.
std::vector<std::wstring> LibraryFolders(std::string_view vdf);

// The folder an app's appmanifest_<appid>.acf names as its "installdir": a plain directory name
// under steamapps\common\. Empty when there is none or it names anything but a plain name.
std::wstring ManifestInstallDir(std::string_view acf);

// Every Steam library on this computer, Steam's own folder first, each once. Empty when Steam is
// not installed.
std::vector<std::wstring> Libraries();

// The folder an app's Workshop downloads go to under one library; it need not exist.
std::wstring WorkshopContentPath(const std::wstring &library, uint32_t appid);

// The Workshop download folders of an app that exist under these libraries, each once.
std::vector<std::wstring> WorkshopContentIn(const std::vector<std::wstring> &libraries, uint32_t appid);

// Where an app is installed under these libraries: the first library whose manifest for it names
// an install folder that exists. Empty when none does.
std::wstring AppInstallDirIn(const std::vector<std::wstring> &libraries, uint32_t appid);

// The same on this computer's libraries.
std::vector<std::wstring> WorkshopContent(uint32_t appid);
std::wstring AppInstallDir(uint32_t appid);

}  // namespace app::steam_library
