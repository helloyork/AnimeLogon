// Steam's library, loaded from one path and nowhere else, and its entry points (steam_flat.h).
#pragma once

#include <string>
#include <type_traits>

#include "steam_flat.h"

namespace steamhelper {

struct Api {
#define STEAM_FLAT_POINTER(name, type) std::add_pointer_t<type> name = nullptr;
    STEAM_FLAT_REQUIRED(STEAM_FLAT_POINTER)
    STEAM_FLAT_OPTIONAL(STEAM_FLAT_POINTER)
#undef STEAM_FLAT_POINTER

    // The newest versioned accessor of each interface the library exports, and its name.
    steam::UGC *(*ugc)() = nullptr;
    steam::Users *(*user)() = nullptr;
    steam::Apps *(*apps)() = nullptr;
    const char *ugcName = "";
};

// Loads the library at `path` (a full path), searching only the folder it is in and System32 for
// what it needs in turn, and looks up every entry point. False when the file cannot be loaded or
// lacks an entry point the helper needs; `why` then says which, in English.
bool Load(const std::wstring &path, Api *api, std::wstring *why);

}  // namespace steamhelper
