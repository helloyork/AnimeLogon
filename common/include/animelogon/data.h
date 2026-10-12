// The data a component can show besides the clock, and the names it shows it by.
//
// Where each comes from:
//   battery.*  the logon screen reads the battery itself;
//   until.*    a date the component gives (its panel's `until`), counted from today;
//   media.*    what is playing in the signed-in person's session, which only that session can
//              read: its agent reports it, the service checks and keeps it (live.h).
//
// A component names data in three ways: a text's value ("{battery.percent}%"), a bar's value
// ("{media.progress}", a fraction from 0 to 1), and a condition ("media.state=playing"). Nothing
// is computed beyond filling these in.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace animelogon::data {

// Plugged: on mains power and not charging -- full, or held below full by a charge limit.
enum class BatteryState { None, Charging, Discharging, Plugged };

struct Battery {
    BatteryState state = BatteryState::None;  // None: no battery, or it could not be read
    int percent = -1;                         // 0..100, -1 when unknown
    int secondsLeft = -1;                     // while discharging; -1 when unknown
};

enum class MediaState { None, Playing, Paused };

struct Media {
    MediaState state = MediaState::None;
    std::wstring title, artist, album, app;  // cleaned (CleanText): one line, at most kMaxText
    double position = 0.0, duration = 0.0;  // seconds; a duration of 0 is unknown
    double rate = 1.0;                       // how fast the position moves while playing
    uint64_t at = 0;                         // when `position` was read, as UTC FILETIME ticks
};

struct Snapshot {
    Battery battery;
    Media media;
};

// The battery as Windows reports it now.
Battery ReadBattery();

struct Date {
    int year = 0, month = 0, day = 0;  // 0: no date
    bool valid() const { return year != 0; }
};
// YYYY-MM-DD, a real day from 1601 to 9999.
bool ParseDate(const std::wstring &text, Date *out);

// --- names ---------------------------------------------------------------------------------

// Text: words for a text ({time}, {media.title}). Fraction: 0..1 for a bar. State: one of a few
// words, only for conditions (battery.state).
enum class Kind { Text, Fraction, State };
// Which source a name belongs to, so the settings app can say what a component shows.
enum class Source { Clock, Battery, Media, Until };

struct Variable {
    const wchar_t *name;
    Kind kind;
    Source source;
    std::vector<std::wstring> states;  // State: the words it can be
    const wchar_t *label;              // what the settings app calls a text showing it
};

// The variable `name` names, or null.
const Variable *Find(const std::wstring &name);
const std::vector<Variable> &Variables();
const wchar_t *LabelOf(Source source);  // e.g. 电量

// --- conditions ----------------------------------------------------------------------------

// name: it has a value. !name: it has none. name=word, name!=word: a state is, or is not, that
// word. One condition only: no "and", no "or".
struct Condition {
    enum class Op { Has, HasNot, Is, IsNot };
    std::wstring name;
    Op op = Op::Has;
    std::wstring value;  // Is, IsNot
};
// False for anything but that shape over a known name; a state's word must be one it can be.
bool ParseCondition(const std::wstring &text, Condition *out);

// --- filling in ----------------------------------------------------------------------------

// What a text, bar or condition is filled in from, besides the clock.
struct Context {
    const Snapshot *snapshot = nullptr;  // null: no battery, nothing playing
    Date until;                          // the panel's
    uint64_t now = 0;                    // UTC FILETIME ticks, for how far playback has got
};

// The words for a non-clock Text variable; empty when it has no value. False if `name` is not
// one.
bool Fill(const std::wstring &name, const Context &context, const SYSTEMTIME &local, std::wstring *out);
// A Fraction variable's value, clamped to 0..1. False when it has none.
bool FractionOf(const std::wstring &name, const Context &context, double *out);
// A State variable's word.
std::wstring StateOf(const std::wstring &name, const Context &context, const SYSTEMTIME &local);

// --- text from elsewhere -------------------------------------------------------------------

// The most characters a text from another program keeps.
constexpr size_t kMaxText = 128;
// Text from another program, made fit for the logon screen: control characters, line breaks,
// and the marks that reorder text (bidirectional overrides and isolates) removed; runs of
// spaces made one; trimmed; cut to `max` characters without splitting a surrogate pair.
std::wstring CleanText(const std::wstring &text, size_t max = kMaxText);

}  // namespace animelogon::data
