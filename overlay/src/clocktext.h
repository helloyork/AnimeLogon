// The clock's words: time and date in the signed-in person's regional format, as the taskbar
// clock shows them. SYSTEM's own "user default" is the one set at Windows setup, so the format
// is read from the console user's hive and remembered for the boot screen.
#pragma once

#include <windows.h>

#include <string>

#include "animelogon/clock.h"

namespace clocktext {

struct Pictures {
    std::wstring locale;
    std::wstring time;
    std::wstring date;  // empty: no date line
};

// The console user's regional format, else the remembered one, else the system's; then the
// clock settings applied to it.
Pictures Resolve(const animelogon::ClockSettings &settings);

std::wstring Time(const Pictures &p, const SYSTEMTIME &t);
std::wstring Date(const Pictures &p, const SYSTEMTIME &t);

}  // namespace clocktext
