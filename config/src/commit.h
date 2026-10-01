// What the unelevated settings window runs as an elevated child of config.exe: importing
// into and removing from the administrators-only library, and the on/off switch.
#pragma once

#include <windows.h>

#include <string>

namespace commit {

// Exit codes the elevated child returns; the window turns them into a sentence.
enum Code { kOk = 0, kFailed = 1, kDeclined = 2, kBadArgs = 3, kNotElevated = 4 };

// Elevated. Checks the transcoder's output in `tempDir` (which must look like the settings
// app's import directory), then copies video.mp4 and audio.wav into videos/<id> and writes
// info.ini there afresh from its parsed contents.
int ImportInto(const std::wstring &id, const std::wstring &tempDir);
// Elevated. Removes videos/<id>.
int Remove(const std::wstring &id);
// Elevated. Parses `tempDir`\skin.xml again (the window has already checked it) and writes it,
// normalised, to skins/<id>/skin.xml.
int ImportSkin(const std::wstring &id, const std::wstring &tempDir);
// Elevated. Removes skins/<id>.
int RemoveSkin(const std::wstring &id);
// Elevated. Turns the logon screen video on (installs and starts the service) or off.
int SwitchOn();
int SwitchOff();

// Runs config.exe elevated with `args` and waits, returning its exit code, or kDeclined if
// the user declined the prompt.
int RunElevated(HWND owner, const std::wstring &args);

}  // namespace commit
