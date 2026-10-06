// Font families the logon screen can use: installed for all users. Fonts installed for one
// account only live in that account's profile, where the logon screen cannot see them.
#pragma once

#include <string>
#include <vector>

namespace fonts {

struct Family {
    std::wstring name;    // in the user's language, for the list
    std::wstring stored;  // the name settings.ini keeps; matched by any localised name
};

std::vector<Family> MachineFamilies();

}  // namespace fonts
