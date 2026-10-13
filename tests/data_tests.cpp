// The data components show besides the clock (data.h), what the signed-in session reports for
// the logon screen (live.h), and the parts of the component format that use them: bars,
// conditions, dates, and the built-in components.
#include "check.h"

#include <cmath>
#include <string>

#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/data.h"
#include "animelogon/live.h"
#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "agentlogon.h"

using namespace animelogon;

TEST(AgentDoesNotRestartIntoALogonThatIsSigningOut) {
    AgentLogonGuard guard;
    CHECK(guard.CanStart(123));
    CHECK(guard.Ended(0x40010004, 123));
    CHECK(!guard.CanStart(123));
    CHECK(!guard.CanStart(0));
    // A fresh authentication id works even for the same user and terminal-session id.
    CHECK(guard.CanStart(124));
    CHECK(!guard.Ended(0xC0000005, 124));
    CHECK(guard.CanStart(124));
    CHECK(!guard.Ended(0, 124));
    CHECK(guard.CanStart(124));
    CHECK(guard.Ended(0xC000026B, 124));
    CHECK(!guard.CanStart(124));
    CHECK(guard.CanStart(125));
}

namespace {

constexpr uint64_t kSecond = 10000000;

SYSTEMTIME Day(WORD year, WORD month, WORD day, WORD hour = 12) {
    SYSTEMTIME t{};
    t.wYear = year;
    t.wMonth = month;
    t.wDay = day;
    t.wHour = hour;
    return t;
}

data::Snapshot Playing() {
    data::Snapshot s;
    s.battery = {data::BatteryState::Discharging, 76, 3 * 3600 + 25 * 60};
    s.media.state = data::MediaState::Playing;
    s.media.title = L"Song";
    s.media.artist = L"Singer";
    s.media.position = 10.0;
    s.media.duration = 200.0;
    s.media.at = 1000 * kSecond;
    return s;
}

std::wstring Fill(const wchar_t *name, const data::Context &c, const SYSTEMTIME &local) {
    std::wstring out;
    CHECK(data::Fill(name, c, local, &out));
    return out;
}

std::string Component(const std::string &settings, const std::string &panel) {
    return "<component format=\"1\" name=\"t\">" + settings + panel + "</component>";
}

bool Parses(const std::string &text, std::wstring *error = nullptr) {
    skin::Skin s;
    return skin::Parse(text, &s, error);
}

}  // namespace

TEST(DataFillsTheBatteryWhatIsPlayingAndTheCountdown) {
    const data::Snapshot s = Playing();
    data::Context c{&s, {2026, 12, 25}, 1030 * kSecond};
    const SYSTEMTIME today = Day(2026, 12, 20);
    CHECK(Fill(L"battery.percent", c, today) == L"76");
    CHECK(Fill(L"battery.remaining", c, today) == L"3:25");
    CHECK(Fill(L"battery.glyph", c, today) == std::wstring(1, (wchar_t)0xE858));  // 8 of 10 bars
    CHECK(Fill(L"media.title", c, today) == L"Song" && Fill(L"media.artist", c, today) == L"Singer");
    CHECK(Fill(L"media.position", c, today) == L"0:40");  // 10 s, and 30 s of playing since
    CHECK(Fill(L"media.duration", c, today) == L"3:20");
    CHECK(Fill(L"media.glyph", c, today) == std::wstring(1, (wchar_t)0xE768));
    CHECK(Fill(L"until.days", c, today) == L"5");
    double f = 0;
    CHECK(data::FractionOf(L"media.progress", c, &f) && std::fabs(f - 0.2) < 1e-9);
    CHECK(data::FractionOf(L"battery.level", c, &f) && std::fabs(f - 0.76) < 1e-9);
    CHECK(data::StateOf(L"battery.state", c, today) == L"discharging");
    CHECK(data::StateOf(L"media.state", c, today) == L"playing");
    CHECK(data::StateOf(L"until.state", c, today) == L"ahead");
    CHECK(data::StateOf(L"until.state", c, Day(2026, 12, 25)) == L"today" && Fill(L"until.days", c, Day(2026, 12, 25)).empty());
    CHECK(data::StateOf(L"until.state", c, Day(2027, 1, 1)) == L"past");

    // Paused: the position stands still. Charging: the charging icon, no time left.
    data::Snapshot p = s;
    p.media.state = data::MediaState::Paused;
    p.battery = {data::BatteryState::Charging, 100, -1};
    c.snapshot = &p;
    CHECK(Fill(L"media.position", c, today) == L"0:10");
    CHECK(Fill(L"media.glyph", c, today) == std::wstring(1, (wchar_t)0xE769));
    CHECK(Fill(L"battery.glyph", c, today) == std::wstring(1, (wchar_t)0xEA93));
    CHECK(Fill(L"battery.remaining", c, today).empty());
    // A position never passes the end.
    p.media.state = data::MediaState::Playing;
    c.now = 5000 * kSecond;
    CHECK(Fill(L"media.position", c, today) == L"3:20");

    // Nothing at all: every one is empty, and the states say so.
    const data::Context none;
    for (const wchar_t *name : {L"battery.percent", L"battery.glyph", L"media.title", L"media.position", L"until.days"})
        CHECK(Fill(name, none, today).empty());
    CHECK(!data::FractionOf(L"media.progress", none, &f) && !data::FractionOf(L"battery.level", none, &f));
    CHECK(data::StateOf(L"battery.state", none, today) == L"none" && data::StateOf(L"until.state", none, today) == L"none");
    // The clock's own names are not data's.
    std::wstring out;
    CHECK(!data::Fill(L"time", c, today, &out) && !data::Fill(L"nope", c, today, &out));
}

TEST(DataFillsTextsWithTheClock) {
    const data::Snapshot s = Playing();
    const data::Context c{&s, {}, 1000 * kSecond};
    const RegionalFormat us = StandardFormat(L"en-US");
    const ClockStyle style;
    SYSTEMTIME t = Day(2026, 10, 2, 22);
    t.wMinute = 8;
    CHECK(FillText(L"{battery.percent}% · {time}", us, style, t, &c) == L"76% · 10:08");
    CHECK(FillText(L"{media.title} - {media.artist}", us, style, t, &c) == L"Song - Singer");
    CHECK(FillText(L"{media.title}", us, style, t).empty());  // no context, no data
}

TEST(DataConditionsAndDates) {
    data::Condition c;
    CHECK(data::ParseCondition(L"media.title", &c) && c.op == data::Condition::Op::Has && c.name == L"media.title");
    CHECK(data::ParseCondition(L"!battery.percent", &c) && c.op == data::Condition::Op::HasNot);
    CHECK(data::ParseCondition(L"battery.state=charging", &c) && c.op == data::Condition::Op::Is && c.value == L"charging");
    CHECK(data::ParseCondition(L"media.state!=playing", &c) && c.op == data::Condition::Op::IsNot);
    CHECK(data::ParseCondition(L"media.progress", &c));
    for (const wchar_t *bad : {L"", L"!", L"nope", L"battery.state=full", L"media.title=Song", L"media.state = playing",
                               L"media.state==playing", L"battery.state=charging,media.state=playing", L"$state"})
        CHECK(!data::ParseCondition(bad, &c));

    data::Date d;
    CHECK(data::ParseDate(L"2028-02-29", &d) && d.year == 2028 && d.month == 2 && d.day == 29);
    for (const wchar_t *bad : {L"2027-02-29", L"2026-13-01", L"2026-00-10", L"2026-1-1", L"26-01-01", L"1600-01-01",
                               L"2026/01/01", L"2026-01-32", L""})
        CHECK(!data::ParseDate(bad, &d));
}

TEST(DataCleansTextFromOtherPrograms) {
    CHECK(data::CleanText(L"  Line one\r\nline\ttwo  ") == L"Line one line two");
    CHECK(data::CleanText(L"a\x202E" L"evil\x202C" L"b\x2066x\x2069\x200F") == L"aevilbx");
    CHECK(data::CleanText(L"bell\x07\x1B[0m") == L"bell[0m");
    CHECK(data::CleanText(L"\xFEFFzero\x0085") == L"zero");
    CHECK(data::CleanText(L"half \xD83D") == L"half" && data::CleanText(L"\xDE00lone") == L"lone");
    CHECK(data::CleanText(L"music \xD83C\xDFB5") == L"music \xD83C\xDFB5");  // a pair stays
    CHECK(data::CleanText(std::wstring(300, L'x')).size() == data::kMaxText);
    // Cut before a pair rather than through it.
    const std::wstring cut = data::CleanText(std::wstring(4, L'x') + L"\xD83C\xDFB5", 5);
    CHECK(cut == L"xxxx");
}

TEST(LiveRoundTripsAndRefusesWhatItDoesNotKnow) {
    live::Live l;
    l.session = 3;
    l.media = Playing().media;
    l.media.album = L"Album\nwith a break";
    l.media.app = L"Player";
    l.media.rate = 1.5;
    const std::string text = live::Serialize(l, true);
    live::Live back;
    std::wstring why;
    CHECK(live::Parse(text, &back, &why));
    CHECK(back.session == 3 && back.media.state == data::MediaState::Playing);
    CHECK(back.media.title == L"Song" && back.media.album == L"Album with a break" && back.media.app == L"Player");
    CHECK(back.media.position == 10.0 && back.media.duration == 200.0 && back.media.rate == 1.5);
    CHECK(back.media.at == 1000 * kSecond);
    // The agent's messages leave the session out.
    CHECK(live::Serialize(l, false).find("session=") == std::string::npos);
    CHECK(live::Parse(live::Serialize(l, false), &back) && back.session == 0xFFFFFFFF);

    // Nothing playing: no words carried.
    CHECK(live::Parse("format=1\nmedia.state=none\nmedia.title=Left over\n", &back) && back.media.title.empty());
    // Text is cleaned whatever sent it.
    CHECK(live::Parse("format=1\r\nmedia.state=paused\r\nmedia.title=\xE2\x80\xAE" "abc\r\n", &back) &&
          back.media.title == L"abc" && back.media.state == data::MediaState::Paused);

    for (const char *bad :
         {"media.state=playing\n", "format=2\n", "format=1\nmedia.state=stopped\n", "format=1\nwhat=1\n",
          "format=1\nmedia.state=playing\nmedia.state=paused\n", "format=1\nmedia.position-ms=-1\n",
          "format=1\nmedia.position-ms=1.5\n", "format=1\nmedia.position-ms=99999999999999\n",
          "format=1\nmedia.rate-permille=100001\n", "format=1\nsession=x\n", "format=1\nno equals\n"})
        CHECK(!live::Parse(bad, &back));
    CHECK(!live::Parse("format=1\nmedia.title=" + std::string(2000, 'x') + "\n", &back));
    CHECK(!live::Parse("format=1\n" + std::string(live::kMaxBytes, '\n'), &back));
}

TEST(ComponentsDrawBarsAndTestConditions) {
    const std::string settings =
        "<settings><date id=\"target\" label=\"Day\" default=\"2027-01-01\"/>"
        "<color id=\"c\" label=\"Colour\" default=\"#FF0000\"/></settings>";
    const std::string panel =
        "<panel until=\"$target\" when=\"until.state!=past\">"
        "<line when=\"media.title\"><text value=\"{media.title}\" when=\"media.state=playing\"/>"
        "<bar value=\"{media.progress}\" length=\"8\" thickness=\"0.1\" color=\"$c\" track=\"#00FF00\" track-opacity=\"0.5\"/>"
        "<text value=\"{until.days}\"/></line></panel>";
    skin::Skin s;
    std::wstring error;
    CHECK(skin::Parse(Component(settings, panel), &s, &error));
    if (!error.empty()) std::printf("  %ls\n", error.c_str());

    skin::Values values{{L"target", L"2026-12-25"}, {L"p1.l1.r1.length", L"12"}};
    const skin::Resolved r = skin::Resolve(s, values);
    CHECK(r.panels.size() == 1);
    if (r.panels.size() != 1 || r.panels[0].lines.size() != 1 || r.panels[0].lines[0].texts.size() != 3) return;
    const skin::Panel &p = r.panels[0];
    CHECK(p.until.year == 2026 && p.until.month == 12 && p.until.day == 25);
    CHECK(p.when && p.when->name == L"until.state" && p.when->op == data::Condition::Op::IsNot);
    CHECK(p.lines[0].when && p.lines[0].when->name == L"media.title");
    const skin::Text &title = p.lines[0].texts[0], &bar = p.lines[0].texts[1], &days = p.lines[0].texts[2];
    CHECK(title.name == L"t1" && !title.bar && title.when && title.when->value == L"playing");
    CHECK(bar.name == L"r1" && bar.bar && bar.value == L"{media.progress}" && bar.length == 12.0f);
    CHECK(bar.thickness == 0.1f && bar.color == 0xFFFF0000 && bar.track == 0xFF00FF00 && bar.trackOpacity == 0.5f);
    CHECK(days.name == L"t2" && !days.when);

    // Bars are parts of their own, adjusted by p1.l1.r1.<attribute>.
    bool barPart = false;
    for (const skin::Part &part : skin::Parts(s))
        if (part.key == L"p1.l1.r1") barPart = part.label == L"播放进度" && !part.adjustments.empty();
    CHECK(barPart);
    CHECK(skin::Accepts(s, L"p1.l1.r1.color", L"#123456") && skin::Accepts(s, L"p1.l1.r1.track-opacity", L"0"));
    CHECK(!skin::Accepts(s, L"p1.l1.r1.value", L"{battery.level}") && !skin::Accepts(s, L"p1.l1.r2.color", L"#123456"));
    CHECK(!skin::Accepts(s, L"p1.when", L"media.title") && !skin::Accepts(s, L"p1.until", L"2026-01-01"));
    CHECK(skin::Accepts(s, L"target", L"2030-06-01") && !skin::Accepts(s, L"target", L"2030-02-30"));

    const auto sources = skin::SourcesOf(s);
    CHECK(sources.size() == 2 && sources[0] == data::Source::Media && sources[1] == data::Source::Until);
    CHECK(skin::SourcesOf(skin::Default()).empty());

    const std::string line = "<panel><line>";
    for (const std::string &bad : {
             line + "<bar value=\"{media.title}\"/></line></panel>",       // not a fraction
             line + "<bar value=\"{media.progress} \"/></line></panel>",   // only the fraction
             line + "<text value=\"{media.progress}\"/></line></panel>",   // a fraction is not words
             line + "<text value=\"{battery.state}\"/></line></panel>",    // nor is a state
             line + "<text value=\"x\" when=\"media.state=stopped\"/></line></panel>",
             line + "<text value=\"x\" when=\"$c\"/></line></panel>",
             line + "<bar/></line></panel>",
             line + "<bar value=\"{media.progress}\" length=\"0\"/></line></panel>",
             std::string("<panel until=\"2026-02-30\"><line><text value=\"x\"/></line></panel>"),
             std::string("<panel until=\"$c\"><line><text value=\"x\"/></line></panel>"),
         })
        CHECK(!Parses(Component(settings, bad)));
    CHECK(!Parses(Component("<settings><date id=\"d\" label=\"D\" default=\"tomorrow\"/></settings>",
                            "<panel><line><text value=\"x\"/></line></panel>")));
}

TEST(BuiltInComponentsComeWithTheProgram) {
    CHECK(BuiltInComponents().size() == 4 && BuiltInComponents()[0] == kClockComponent);
    for (const std::wstring &id : BuiltInComponents()) {
        skin::Skin s;
        std::wstring why;
        CHECK(IsBuiltInComponent(id) && IsComponentId(id));
        CHECK(LoadComponent(id, &s, &why) && !s.name.empty());
        if (!why.empty()) std::printf("  %ls: %ls\n", id.c_str(), why.c_str());
    }
    skin::Skin battery, media, countdown;
    CHECK(BuiltInComponent(L"battery", &battery) && skin::SourcesOf(battery) == std::vector{data::Source::Battery});
    CHECK(BuiltInComponent(L"media", &media) && skin::SourcesOf(media) == std::vector{data::Source::Media});
    CHECK(BuiltInComponent(L"countdown", &countdown) && skin::SourcesOf(countdown) == std::vector{data::Source::Until});
    CHECK(countdown.Find(L"target") && countdown.Find(L"target")->kind == skin::SettingKind::Date);
    CHECK(!IsBuiltInComponent(L"weather") && !IsComponentId(L"weather"));
    // They come first in the list, before anything imported.
    const auto list = ListComponents();
    CHECK(list.size() >= 4 && list[1].id == L"battery" && list[1].builtIn && list[3].id == L"countdown");
}

TEST(SettingsKeepWhetherWhatIsPlayingIsShown) {
    Settings s;
    CHECK(s.media);
    std::vector<std::wstring> problems;
    Settings off = ParseSettings(L"theme = default\r\nmedia = false\r\n", &problems);
    CHECK(!off.media && problems.empty());
    CHECK(SerializeSettings(off).find(L"media = false") != std::wstring::npos);
    Settings bad = ParseSettings(L"theme = default\r\nmedia = maybe\r\n", &problems);
    CHECK(bad.media && problems.size() == 1);
}
