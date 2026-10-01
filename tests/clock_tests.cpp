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
    s.clock.anchor = ClockAnchor::BottomRight;
    s.clock.size = ClockSize::Huge;
    s.clock.font = L"Microsoft YaHei UI";
    s.clock.color = 0x12ABEF;
    s.clock.date = DateStyle::Weekday;
    s.clock.hour24 = true;
    s.clock.language = L"ja-JP";
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(SerializeSettings(s), &problems);
    CHECK(problems.empty());
    CHECK(!t.clock.enabled && t.clock.displays == ClockDisplays::Primary);
    CHECK(t.clock.anchor == ClockAnchor::BottomRight && t.clock.size == ClockSize::Huge);
    CHECK(t.clock.font == L"Microsoft YaHei UI" && t.clock.color == 0x12ABEF);
    CHECK(t.clock.date == DateStyle::Weekday && t.clock.hour24 && t.clock.language == L"ja-JP");
}

TEST(ClockSettingsDefaultsAndBadValues) {
    const Settings d;
    CHECK(d.clock.enabled && d.clock.displays == ClockDisplays::Auto && d.clock.font.empty());
    std::vector<std::wstring> problems;
    const Settings t = ParseSettings(L"clock_color = red\r\nclock_language = fr-FR\r\nclock_font = a\\b\r\n"
                                     L"clock_position = middle\r\n",
                                     &problems);
    CHECK(problems.size() == 4);
    CHECK(t.clock.color == 0xFFFFFF && t.clock.language.empty() && t.clock.font.empty());
    CHECK(t.clock.anchor == ClockAnchor::Top);
}
