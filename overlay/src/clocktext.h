// The clock's regional format: the signed-in person's, as the taskbar clock uses it. SYSTEM's
// own "user default" is the one set at Windows setup, so the format is read from the console
// user's hive and remembered for the boot screen.
#pragma once

#include "animelogon/clock.h"

namespace clocktext {

// The console user's regional format, else the remembered one, else the system's.
animelogon::RegionalFormat UserFormat();

}  // namespace clocktext
