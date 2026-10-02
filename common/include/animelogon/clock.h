// The logon clock: which skin draws it and where, and the text rules behind its time and date.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>

#include "animelogon/skin.h"

namespace animelogon {

// Auto is the display Windows puts its password box on.
enum class ClockDisplays { Auto, Primary, All };
// Auto follows the regional format.
enum class ClockHours { Auto, H12, H24 };
// Short is the weekday with the month and day; Long is the regional long date.
enum class ClockDate { Short, Long, None };

// What the clock says, whichever skin shows it.
struct ClockStyle {
    ClockHours hours = ClockHours::Auto;
    bool ampm = true;     // the AM/PM marker, when the time is 12-hour
    ClockDate date = ClockDate::Short;
    std::wstring locale;  // empty: the regional format's own language
};

struct ClockSettings {
    // Legacy, until the overlay and the settings app use themes (see settings.h): `clock`,
    // `clock_displays`, `skin` and `skin.<id>.*`.
    bool enabled = true;
    ClockDisplays displays = ClockDisplays::Auto;
    // Global, whatever the theme: `clock_hours`, `clock_ampm`, `clock_date`, `clock_language`.
    ClockStyle style;
    // Legacy, as above.
    std::wstring skin = L"default";               // "default" or a skin's library id
    std::map<std::wstring, skin::Values> values;  // by skin id

    const skin::Values &ValuesFor(const std::wstring &skinId) const;
};

// The languages the clock can be switched to, besides the regional format's own.
bool IsClockLanguage(const std::wstring &locale);
// A font family name as settings.ini may carry it.
bool IsFontFamilyName(const std::wstring &name);

// Time pictures. Without the AM/PM marker and the spaces that set it apart; 24-hour, also
// without the marker; 12-hour, keeping a marker if there is one.
std::wstring WithoutMarkerPicture(const std::wstring &picture);
std::wstring To24HourPicture(const std::wstring &picture);
std::wstring To12HourPicture(const std::wstring &picture);
bool HasMarker(const std::wstring &picture);
// A long date picture that names the weekday. Pictures that already do are kept.
std::wstring WeekdayDatePicture(const std::wstring &locale, const std::wstring &longDate);
// A date picture without the year, and without the words and separators that went with it.
std::wstring WithoutYearPicture(const std::wstring &picture);

// A regional format, as remembered for the logon screen nobody is signed in to.
struct RegionalFormat {
    std::wstring locale;     // e.g. zh-CN
    std::wstring shortTime;  // e.g. H:mm
    std::wstring longDate;   // e.g. yyyy'年'M'月'd'日'
};
std::string SerializeRegionalFormat(const RegionalFormat &f);
bool ParseRegionalFormat(const std::string &text, RegionalFormat *f);
// A locale's own standard format.
RegionalFormat StandardFormat(const std::wstring &locale);
// `user`, unless `locale` names another language: then that language's standard format.
RegionalFormat FormatFor(const RegionalFormat &user, const std::wstring &locale);

// A skin text with its data filled in: {time}, {date} ... `format` is already in the style's
// language.
std::wstring FillText(const std::wstring &text, const RegionalFormat &format, const ClockStyle &style,
                      const SYSTEMTIME &t);

const wchar_t *ToString(ClockDisplays displays);
bool Parse(const std::wstring &text, ClockDisplays *out);
const wchar_t *ToString(ClockHours hours);
bool Parse(const std::wstring &text, ClockHours *out);
const wchar_t *ToString(ClockDate date);
bool Parse(const std::wstring &text, ClockDate *out);
bool ParseColor(const std::wstring &text, uint32_t *rgb);  // #RRGGBB
std::wstring FormatColor(uint32_t rgb);

}  // namespace animelogon
