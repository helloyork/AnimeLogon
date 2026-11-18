// animelogon.exe's command line: which of its roles it is started in, and the settings app's own
// arguments. Apart from main.cpp so that the tests can read command lines too.
//
// The settings app's arguments, in any order:
//   --sandbox <dir>  a window apart from what is installed, on a copy of the data under <dir> (paths.h)
//   --show <what>    comes up at what the icon asked for (IsShowTarget)
//   --tray           the icon instead of the window, in the sandbox if one is given (tray.h)
//   <file>           a theme package to import, as a double click on an .altheme file asks for
//                    (installer/src/assoc.h); in the window already open, if there is one
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace app {

enum class Role {
    App,           // the settings app's window, or with `tray` the icon (Arguments)
    Agent,         // --agent, alone: the service's agent in the signed-in person's session (agent.h)
    RestoreLockScreen,  // --restore-lock-screen, alone: puts back the person's own lock screen picture (lockscreen.h)
    Helper,        // the elevated helper: a first argument that starts with '-' and is none of the app's (commit.h)
    Unrecognized,  // the app's arguments, but not as it takes them
};

struct Arguments {
    bool tray = false;
    bool sandbox = false;
    std::wstring dir;   // the sandbox's
    std::wstring show;  // what to come up at
    std::wstring open;  // the file to import, as given
};

// The arguments after the program's own name. `out` is filled for Role::App.
Role ReadArguments(const std::vector<std::wstring> &args, Arguments *out);

// What the settings app can be asked to show: itself as it is (empty), the switch that turns
// AnimeLogon on and off ("switch"), the lock-screen preview of the theme in use ("lock").
bool IsShowTarget(std::wstring_view what);

// Whether `path` is named as a theme package is: ending in .altheme, in any case. A file given on
// the command line that is not is refused with the import's own message, without being read.
bool IsPackagePath(std::wstring_view path);

}  // namespace app
