// The logon screen moving through a list of themes. settings.ini's `rotation` keys say which themes
// and how (settings.h); rotation.ini says where it has got to.
//
// rotation.ini is written only by the overlay of the session at the console (SYSTEM), and only
// while that session is in use -- signed in and unlocked -- so each move happens at a desktop, and
// the new theme's sign-in picture is ready well before the next credential screen (signin.h).
// Users may read it: the settings app and the icon show which theme is up.
//
// The theme shown is the one the rotation has got to, unless a theme was chosen by hand since it
// last looked (settings.ini's `theme` differs from `chosen`): that one is shown at once, and when
// it is in the list the rotation goes on from it. Whoever chooses a theme that is not in the list
// turns the rotation off in the same write, or the rotation would take the screen back.
//
// rotation.ini, one key=value per line, UTF-8, every key at most once:
//   format=1
//   themes=<id>,<id>,...  the list it was made for: rotation_themes, installed themes only
//   current=<id>          the theme the rotation has got to
//   chosen=<id>           settings.ini's `theme` when it last looked
//   since=<ticks>         when `current` came up, in UTC FILETIME ticks
//   screen=<ticks>        the latest logon screen (rotation_trigger = lock)
//   bag=<id>,<id>,...     rotation_order = shuffle: the themes still to come this round
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "animelogon/settings.h"

namespace animelogon::rotation {

constexpr uint64_t kTicksPerSecond = 10'000'000;
constexpr size_t kMaxBytes = 16 * 1024;

struct State {
    std::vector<std::wstring> themes;
    std::wstring current;
    std::wstring chosen;
    uint64_t since = 0;
    uint64_t screen = 0;
    std::vector<std::wstring> bag;
};

// %ProgramData%\AnimeLogon\rotation.ini
std::wstring Path();

std::string Serialize(const State &state);
bool Parse(std::string_view text, State *out, std::wstring *why = nullptr);
// Reads rotation.ini. False, leaving an empty state, when it is missing, could have been written by
// anyone but an administrator, or is malformed.
bool Load(State *out, std::wstring *why = nullptr);
// SYSTEM or elevated callers only.
DWORD Save(const State &state);

// The current time, in UTC FILETIME ticks.
uint64_t Now();

// The theme the logon screen shows with these settings.
std::wstring Shown(const Settings &settings, const State &state);
// `settings` with `theme` set to Shown(): what the overlay draws, and what the agent and the
// settings app take as the theme in use.
Settings WithShown(Settings settings, const State &state);
// The same, reading rotation.ini when the rotation is on.
Settings WithShown(Settings settings);
// LoadSettings(false), and rotation.ini when the rotation is on.
Settings LoadShown(std::wstring *why = nullptr);

// The themes of rotation_themes that can be shown, in their order; `installed` says whether one is.
std::vector<std::wstring> Playable(const Settings &settings, const std::function<bool(const std::wstring &)> &installed);

// Brings `state` up to date with the settings at `now`, and moves on to the next theme when it is
// time. Returns true when `state` changed and is to be saved; `moved` says whether the theme shown
// changed. `pick(n)` gives a number below n, for the shuffle.
bool Advance(const Settings &settings, const std::vector<std::wstring> &playable, uint64_t now, State *state,
             bool *moved, const std::function<size_t(size_t)> &pick);

// Notes in rotation.ini that a logon screen came up now, when the rotation moves on after each one.
// From the overlay as it covers a logon screen; SYSTEM only.
void NoteScreen(const Settings &settings);

// When the rotation moves on next, in UTC FILETIME ticks: 0 when it is not counting time (off,
// fewer than two themes, or moving after each logon screen).
uint64_t NextAt(const Settings &settings, const State &state);

}  // namespace animelogon::rotation
