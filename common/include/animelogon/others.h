// Other programs that replace the logon screen the way AnimeLogon does. Two of them on at once
// fight over the same Windows settings and the same screen, so AnimeLogon switches the others off
// when it is installed or switched on. Today there is one such program; others.cpp names it.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace animelogon::others {

struct Program {
    std::wstring name;   // as Apps & features shows it
    bool on = false;     // installed and not switched off
};

// Every such program installed on this machine, on or off. Reads only; anyone may ask.
std::vector<Program> Installed();
bool AnyOn();

// Switches every installed one off as its own settings app would, and puts back the Windows
// values it set, so that AnimeLogon records the machine's own values rather than its. Settings
// and files are kept; the program can be switched on again from its own settings. `ownImage` is
// AnimeLogon's sign-in background, left in place when Windows already points at it. Run it before
// machine::TurnOn. Administrators only. Fails only when a program cannot be switched off at all.
DWORD TurnOff(const std::wstring &ownImage);

}  // namespace animelogon::others
