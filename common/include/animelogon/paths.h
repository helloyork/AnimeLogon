// Where AnimeLogon keeps its files and registry state.
#pragma once

#include <string>

namespace animelogon::paths {

// A settings window started with --sandbox <dir> (app/src/main.cpp) keeps everything it reads
// and writes under <dir>, laid out as on a machine: <dir>\ProgramData\AnimeLogon for the data
// and <dir>\LocalAppData\AnimeLogon for the person's own files. It is for looking at a build
// apart from what is installed. Only that unelevated window sets it; nothing elevated or run by
// the system ever does. Empty outside a sandbox.
void UseSandbox(const std::wstring &root);
const std::wstring &Sandbox();

// %ProgramData%\AnimeLogon. Created by the installer with an administrators-only DACL.
std::wstring DataDir();
std::wstring LogDir();
std::wstring LogPath(const wchar_t *name);
std::wstring SettingsPath();
std::wstring BackgroundPath();
// Present while the logon screen video is switched off from the logon screen itself.
std::wstring PausedMarkerPath();

// %LOCALAPPDATA%\AnimeLogon, for the settings app's own logs and work files.
std::wstring UserDataDir();

// The directory holding the running executable, with a trailing backslash.
std::wstring ModuleDir();

// HKLM\SOFTWARE\AnimeLogon.
constexpr const wchar_t *kRegistryKey = L"SOFTWARE\\AnimeLogon";
constexpr const wchar_t *kServiceName = L"AnimeLogon";

// Creates every missing directory in `path`. Returns false if the last one is not there.
bool CreateDirectories(const std::wstring &path);

// A drive-letter path with backslashes only and no empty, ".", ".." or stream components,
// so that it has exactly one spelling.
bool IsPlainAbsolute(const std::wstring &path);
// True when `path` is `root` or lies beneath it. Both must be plain absolute paths.
bool IsWithin(const std::wstring &path, const std::wstring &root);

}  // namespace animelogon::paths
