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
    s.clock.enabled = false;
    s.clock.displays = ClockDisplays::Primary;
    s.clock.skin = L"0123456789abcdef";
    s.clock.values[L"default"][L"color"] = L"#12ABEF";
    s.clock.values[L"0123456789abcdef"][L"font"] = L"Microsoft YaHei UI";
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(!t.clock.enabled && t.clock.displays == ClockDisplays::Primary && t.clock.skin == L"0123456789abcdef");
    CHECK(t.clock.ValuesFor(L"default").at(L"color") == L"#12ABEF");
    CHECK(t.clock.ValuesFor(L"0123456789abcdef").at(L"font") == L"Microsoft YaHei UI");
    CHECK(t.clock.ValuesFor(L"fedcba9876543210").empty());
}

TEST(ClockSettingsDefaultsAndBadValues) {
    const Settings d;
    CHECK(d.clock.enabled && d.clock.displays == ClockDisplays::Auto && d.clock.skin == L"default");
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(L"skin = ../x\r\nskin.default = 1\r\nskin.default.Color = #FFFFFF\r\n"
                                     L"skin.nothex.color = #FFFFFF\r\nskin.default.color = a\x01\r\n",
                                     &problems);
    CHECK(problems.size() == 5);
    CHECK(t.clock.skin == L"default" && t.clock.values.empty());
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
    CHECK(FillText(L"{time}", us, skin::Hours::Auto, t) == L"10:08");
    CHECK(FillText(L"{ampm}", us, skin::Hours::Auto, t) == L"PM");
    CHECK(FillText(L"{time}", us, skin::Hours::H24, t) == L"22:08");
    CHECK(FillText(L"{ampm}", us, skin::Hours::H24, t).empty());
    CHECK(FillText(L"{date}", us, skin::Hours::Auto, t) == L"Friday, October 2");
    CHECK(FillText(L"{date.long}", us, skin::Hours::Auto, t) == L"Friday, October 2, 2026");
    CHECK(FillText(L"{day} {month} {year} ({weekday})", us, skin::Hours::Auto, t) == L"2 October 2026 (Friday)");
    const RegionalFormat cn{L"zh-CN", L"H:mm", L"yyyy'\x5E74'M'\x6708'd'\x65E5'"};
    CHECK(FillText(L"{time}", cn, skin::Hours::Auto, t) == L"22:08");
    CHECK(FillText(L"{ampm}", cn, skin::Hours::Auto, t).empty());
    CHECK(FillText(L"{time}", cn, skin::Hours::H12, t) == L"10:08");
    CHECK(FillText(L"{date}", cn, skin::Hours::Auto, t) == L"10\x6708" L"2\x65E5 \x661F\x671F\x4E94");
    CHECK(FormatFor(cn, L"zh-CN").shortTime == L"H:mm" && FormatFor(cn, L"en-US").locale == L"en-US");
}
