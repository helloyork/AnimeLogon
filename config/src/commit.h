// The command line of config.exe's elevated child: what the unelevated settings window runs to
// change the administrators-only stores (store.h) and the on/off switch.
//
//   --commit-wallpaper <id> <dir>    install a staged wallpaper as wallpapers\<id>
//   --remove-wallpaper <id>
//   --commit-component <id> <dir>    install a staged component as components\<id>
//   --remove-component <id>
//   --commit-theme <id> <dir>        install a staged theme (installed form) as themes\<id>
//   --remove-theme <id>
//   --commit-package <dir>           install a staged theme package under new ids
//   --switch-on, --switch-off        turn the logon screen overlay on or off
//
// <dir> is an import directory (store::IsImportDir). One command line may hold several
// commands one after another, so that one prompt covers, for example, removing a theme together
// with the wallpaper and components only it used. Every command is checked before any runs;
// they then run in order, and the first failure stops the rest.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace store {
class Target;
}

namespace commit {

// Exit codes the elevated child returns; the window turns them into a sentence.
enum Code { kOk = 0, kFailed = 1, kDeclined = 2, kBadArgs = 3, kNotElevated = 4 };

struct Command {
    std::wstring name;               // e.g. --commit-package
    std::vector<std::wstring> args;  // its arguments
};

// Splits the arguments after the program name into commands. kBadArgs, with `out` left empty,
// for an unknown command, a missing argument, or an id or directory of the wrong shape.
int Parse(const std::vector<std::wstring> &args, std::vector<Command> *out);

// Runs one parsed command against `target` (store::Disk() for the real stores).
int Execute(const Command &command, const store::Target &target);

// The elevated child's whole run: parse, refuse unless running with administrator rights,
// then execute in order. Logs to config.log.
int Run(const std::vector<std::wstring> &args);

// An argument as CommandLineToArgvW reads it back: quoted when it has spaces or quotes.
std::wstring Quote(const std::wstring &arg);

// Runs config.exe elevated with `args` and waits, returning its exit code, or kDeclined if
// the user declined the prompt.
int RunElevated(HWND owner, const std::wstring &args);

}  // namespace commit
