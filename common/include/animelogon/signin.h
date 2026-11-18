// The sign-in background: the picture Windows draws behind its credential screen.
//
// Windows draws there the lock screen picture of the person whose tile is selected (as long as
// "Show lock screen background picture on the sign-in screen" is on, machine.h). Set through
// Windows.System.UserProfile.LockScreen from that person's own session, Windows prepares the
// picture before the call returns -- its copy under %ProgramData%\Microsoft\Windows\SystemData\<SID>
// is made then -- so the next credential screen has it from its first frame, and one changed while
// a credential screen is up is cross-faded in by Windows itself. (A picture named through the
// PersonalizationCSP policy instead is only prepared once a credential screen is already being
// drawn: every change of it left a credential screen without a picture, for seconds at a boot.)
//
// The overlay (SYSTEM) bakes the picture of the theme shown into Dir() and names it in signin.ini
// (Path()); the agent in the signed-in person's session (app/src/agent.cpp) sets it as their lock
// screen picture whenever it differs from what they have. Both are administrators-only files that
// Users may read. Each picture's name comes from its key, so a changed picture is a new file, never
// the same file with other bytes.
//
// signin.ini, one key=value per line, UTF-8, every key at most once:
//   format=1
//   key=<the bake's key>
//   theme=<theme id>          the theme it is the picture of
//   image=<16 hex digits>.png the picture's file name in Dir()
#pragma once

#include <string>
#include <string_view>

namespace animelogon::signin {

constexpr size_t kMaxBytes = 8 * 1024;
// How many pictures Dir() keeps: the ones the theme list moves through come back without baking.
constexpr size_t kKeep = 8;

struct Want {
    std::wstring key;
    std::wstring theme;
    std::wstring image;  // the full path, in Dir()
};

// %ProgramData%\AnimeLogon\signin.ini, and the directory of the pictures.
std::wstring Path();
std::wstring Dir();
// Where the picture with this key is kept.
std::wstring ImageFor(const std::wstring &key);

std::string Serialize(const Want &want);
// The image must be named as ImageFor names pictures; it is given back as its full path in Dir().
bool Parse(std::string_view text, Want *out, std::wstring *why = nullptr);
// Reads signin.ini, refusing one an administrator did not write.
bool Load(Want *out, std::wstring *why = nullptr);

}  // namespace animelogon::signin
