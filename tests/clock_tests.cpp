#include "check.h"

#include "animelogon/clock.h"
#include "animelogon/settings.h"

using namespace animelogon;

TEST(ClockTwentyFourHour) {
    CHECK(To24HourPicture(L"h:mm tt") == L"H:mm");
    CHECK(To24HourPicture(L"tt h:mm") == L"H:mm");
    CHECK(To24HourPicture(L"tt hh:mm") == L"HH:mm");
    CHECK(To24HourPicture(L"H:mm") == L"H:mm");
    CHECK(To24HourPicture(L"H.mm") == L"H.mm");
    CHECK(To24HourPicture(L"'h' h:mm") == L"'h' H:mm");  // quoted text is left alone
    CHECK(To24HourPicture(L"tt") == L"HH:mm");
}

TEST(ClockWeekdayPicture) {
    CHECK(WeekdayDatePicture(L"en-US", L"dddd, MMMM d, yyyy") == L"dddd, MMMM d, yyyy");
    CHECK(WeekdayDatePicture(L"zh-CN", L"yyyy'年'M'月'd'日'") == L"yyyy'年'M'月'd'日' dddd");
    CHECK(WeekdayDatePicture(L"ja-JP", L"yyyy'年'M'月'd'日'") == L"yyyy'年'M'月'd'日' dddd");
    CHECK(WeekdayDatePicture(L"de-DE", L"d. MMMM yyyy") == L"dddd, d. MMMM yyyy");
    CHECK(WeekdayDatePicture(L"en-GB", L"'dddd' d MMMM yyyy") == L"dddd, 'dddd' d MMMM yyyy");
}

TEST(ClockWithoutYear) {
    CHECK(WithoutYearPicture(L"dddd, MMMM d, yyyy") == L"dddd, MMMM d");
    CHECK(WithoutYearPicture(L"yyyy'年'M'月'd'日'") == L"M'月'd'日'");
    CHECK(WithoutYearPicture(L"yyyy'年'M'月'd'日' dddd") == L"M'月'd'日' dddd");
    CHECK(WithoutYearPicture(L"yyyy'년' M'월' d'일' dddd") == L"M'월' d'일' dddd");
    CHECK(WithoutYearPicture(L"dddd, d. MMMM yyyy") == L"dddd, d. MMMM");
    CHECK(WithoutYearPicture(L"dddd d MMMM yyyy") == L"dddd d MMMM");
    CHECK(WithoutYearPicture(L"dddd, d' de 'MMMM' de 'yyyy") == L"dddd, d' de 'MMMM");
    CHECK(WithoutYearPicture(L"d MMMM yyyy 'г.'") == L"d MMMM");
    CHECK(WithoutYearPicture(L"dddd, MMMM d") == L"dddd, MMMM d");
    CHECK(WithoutYearPicture(L"'yyyy' d MMMM") == L"'yyyy' d MMMM");
}

TEST(ClockRegionalFormatRoundTrip) {
    RegionalFormat f{L"zh-CN", L"H:mm", L"yyyy'年'M'月'd'日'"}, g;
    CHECK(ParseRegionalFormat(SerializeRegionalFormat(f), &g));
    CHECK(g.locale == f.locale && g.shortTime == f.shortTime && g.longDate == f.longDate);
    CHECK(!ParseRegionalFormat("zh-CN\nH:mm\n", &g));
    CHECK(!ParseRegionalFormat("zh CN\nH:mm\nd\n", &g));
    CHECK(!ParseRegionalFormat("zh-CN\n\nd\n", &g));
}

TEST(ClockSettingsRoundTrip) {
    Settings s;
    s.clock.hours = ClockHours::H24;
    s.clock.ampm = false;
    s.clock.date = ClockDate::None;
    s.clock.locale = L"ja-JP";
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(t.clock.hours == ClockHours::H24 && !t.clock.ampm && t.clock.date == ClockDate::None);
    CHECK(t.clock.locale == L"ja-JP");
}

TEST(ClockSettingsDefaultsAndBadValues) {
    const Settings d;
    CHECK(d.clock.hours == ClockHours::Auto && d.clock.ampm && d.clock.date == ClockDate::Short);
    CHECK(d.clock.locale.empty());
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(L"theme = default\r\nclock_hours = 13\r\nclock_date = soon\r\n"
                                     L"clock_language = xx-YY\r\nclock_ampm = maybe\r\n",
                                     &problems);
    CHECK(problems.size() == 4);
    CHECK(t.clock.hours == ClockHours::Auto && t.clock.ampm && t.clock.date == ClockDate::Short && t.clock.locale.empty());
}

TEST(ClockTimePictures) {
    CHECK(WithoutMarkerPicture(L"h:mm tt") == L"h:mm");
    CHECK(WithoutMarkerPicture(L"tt h:mm") == L"h:mm");
    CHECK(WithoutMarkerPicture(L"H:mm") == L"H:mm");
    CHECK(To12HourPicture(L"HH:mm") == L"h:mm");
    CHECK(To12HourPicture(L"H.mm") == L"h.mm");
    CHECK(To12HourPicture(L"'H' H:mm") == L"'H' h:mm");
    CHECK(HasMarker(L"h:mm tt") && !HasMarker(L"H:mm") && !HasMarker(L"'t' H:mm"));
}

TEST(ClockFillsText) {
    SYSTEMTIME t{};
    t.wYear = 2026;
    t.wMonth = 10;
    t.wDay = 2;
    t.wDayOfWeek = 5;
    t.wHour = 22;
    t.wMinute = 8;
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    ClockStyle style, h24, h12, quiet, full, none;
    h24.hours = ClockHours::H24;
    h12.hours = ClockHours::H12;
    quiet.ampm = false;
    full.date = ClockDate::Long;
    none.date = ClockDate::None;
    CHECK(FillText(L"{time}", us, style, t) == L"10:08");
    CHECK(FillText(L"{ampm}", us, style, t) == L"PM");
    CHECK(FillText(L"{ampm}", us, quiet, t).empty() && FillText(L"{time}", us, quiet, t) == L"10:08");
    CHECK(FillText(L"{time}", us, h24, t) == L"22:08");
    CHECK(FillText(L"{ampm}", us, h24, t).empty());
    CHECK(FillText(L"{date}", us, style, t) == L"Friday, October 2");
    CHECK(FillText(L"{date}", us, full, t) == L"Friday, October 2, 2026");
    CHECK(FillText(L"{date}", us, none, t).empty() && FillText(L"{weekday} {year}", us, none, t) == L" ");
    CHECK(FillText(L"{date.long}", us, style, t) == L"Friday, October 2, 2026");
    CHECK(FillText(L"{day} {month} {year} ({weekday})", us, style, t) == L"2 October 2026 (Friday)");
    const RegionalFormat cn{L"zh-CN", L"H:mm", L"yyyy'\x5E74'M'\x6708'd'\x65E5'"};
    CHECK(FillText(L"{time}", cn, style, t) == L"22:08");
    CHECK(FillText(L"{ampm}", cn, style, t).empty());
    CHECK(FillText(L"{time}", cn, h12, t) == L"10:08");
    CHECK(FillText(L"{date}", cn, style, t) == L"10\x6708" L"2\x65E5 \x661F\x671F\x4E94");
    CHECK(FormatFor(cn, L"zh-CN").shortTime == L"H:mm" && FormatFor(cn, L"en-US").locale == L"en-US");
}
