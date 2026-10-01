// The logon clock: its settings, and the pure text rules behind its time and date formats.
#pragma once

#include <cstdint>
#include <string>

namespace animelogon {

enum class ClockAnchor { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
enum class ClockSize { Small, Medium, Large, Huge };
// Auto is the display Windows puts its password box on.
enum class ClockDisplays { Auto, Primary, All };
enum class DateStyle { None, Long, Weekday };

struct ClockSettings {
    bool enabled = true;
    ClockDisplays displays = ClockDisplays::Auto;
    ClockAnchor anchor = ClockAnchor::Top;
    ClockSize size = ClockSize::Medium;
    std::wstring font;          // a family installed for all users; empty is the default
    uint32_t color = 0xFFFFFF;  // 0xRRGGBB
    DateStyle date = DateStyle::Long;
    bool hour24 = false;        // false follows the regional format
    std::wstring language;      // empty follows the regional format
};

// The languages the clock can be switched to, besides the regional format's own.
bool IsClockLanguage(const std::wstring &locale);
// A font family name as settings.ini may carry it.
bool IsFontFamilyName(const std::wstring &name);

// A Windows time picture with the hour made 24-hour and any AM/PM marker removed.
std::wstring To24HourPicture(const std::wstring &picture);
// A long date picture that names the weekday. Pictures that already do are kept.
std::wstring WeekdayDatePicture(const std::wstring &locale, const std::wstring &longDate);

// A regional format, as remembered for the logon screen nobody is signed in to.
struct RegionalFormat {
    std::wstring locale;     // e.g. zh-CN
    std::wstring shortTime;  // e.g. H:mm
    std::wstring longDate;   // e.g. yyyy'年'M'月'd'日'
};
std::string SerializeRegionalFormat(const RegionalFormat &f);
bool ParseRegionalFormat(const std::string &text, RegionalFormat *f);

const wchar_t *ToString(ClockAnchor anchor);
const wchar_t *ToString(ClockSize size);
const wchar_t *ToString(ClockDisplays displays);
const wchar_t *ToString(DateStyle style);
bool Parse(const std::wstring &text, ClockAnchor *out);
bool Parse(const std::wstring &text, ClockSize *out);
bool Parse(const std::wstring &text, ClockDisplays *out);
bool Parse(const std::wstring &text, DateStyle *out);
bool ParseColor(const std::wstring &text, uint32_t *rgb);  // #RRGGBB
std::wstring FormatColor(uint32_t rgb);

}  // namespace animelogon
