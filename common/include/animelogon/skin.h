// The component format: what is drawn over the wallpaper, declared in a small XML file whose
// root element is <component>. The code calls a component a skin. A component is structure
// (elements and their attributes) plus settings the person can adjust; the settings' values
// come from a theme and from settings.ini. Nothing in a component computes: an attribute is either a literal, or "$id"
// for the value of one of its settings, and text may name data such as {time} or
// {battery.percent} (data.h). A bar shows a fraction such as {media.progress}, a spectrum the
// sound the output device plays (spectrum.h), and a panel, line, text, bar or spectrum may be
// drawn only `when` a condition on the data holds. Beyond the
// component's own settings, any attribute that shapes how an element looks can be adjusted by
// hand, element by element; those values come from the same places.
//
// A component's structure can put any words on the screen, so it is only read from where
// administrators alone can write. Setting values and adjustments carry no free text: a
// colour, a number in range, a font, or one of a fixed set of words.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "animelogon/data.h"
#include "animelogon/xml.h"

namespace animelogon::skin {

// Colors: a list of colours at places along a gradient (Stop, ParseStops).
enum class SettingKind { Choice, Toggle, Number, Color, Font, Date, Colors };

struct Option {
    std::wstring value, label;
};

struct Setting {
    SettingKind kind = SettingKind::Choice;
    std::wstring id, label, detail, fallback;  // `fallback` is the default value
    std::vector<Option> options;       // Choice; a Toggle has "on" then "off"
    double min = 0, max = 100, step = 1;  // Number
};

struct Skin {
    std::wstring name, author;
    std::vector<Setting> settings;
    xml::Element document;

    const Setting *Find(const std::wstring &id) const;
};

// Values by setting id. Missing or unusable values fall back to the setting's default.
using Values = std::map<std::wstring, std::wstring>;

bool Parse(std::string_view utf8, Skin *skin, std::wstring *error);
// The skin as it is stored: parsed and written again, so nothing unchecked survives. The root
// element is always written as <component>.
std::string Normalize(const Skin &skin);
// The skin that comes with AnimeLogon.
const Skin &Default();
std::string DefaultText();

bool IsValue(const Setting &setting, const std::wstring &value);
std::wstring ValueOf(const Setting &setting, const Values &values);
// The shape of a setting id. "ref" and "visible" have this shape but no component may use
// them: settings.ini gives them to the component instance itself.
bool IsSettingId(const std::wstring &id);

// --- adjusting one attribute of one element ----------------------------------------------
//
// Elements are named by where they are: p1 is the first panel, p1.l2.t1 the first text on its
// second line, p1.l2.r1 the first bar on it, p1.l2.a1 the first spectrum on it, p1.s1 its first
// shadow, p1.b its backdrop. An adjustment is stored under the
// element's name and the attribute's, as in p1.l1.t1.weight, and wins over the skin's own
// value for that attribute.

enum class Control { Slider, Choice, Color, Font, Toggle, Colors };

struct Adjustment {
    std::wstring key;  // e.g. p1.l1.t1.weight
    std::wstring label, detail;
    Control control = Control::Slider;
    double min = 0, max = 1, step = 0.01;  // Slider
    std::vector<Option> options;           // Choice
};

// One element as the settings app offers it.
struct Part {
    std::wstring key, label;  // e.g. p1.l1.t1, "时间"
    std::vector<Adjustment> adjustments;
};

std::vector<Part> Parts(const Skin &skin);
// The value an adjustment shows: adjusted by hand, from a setting, as written, or the default.
// A weight is always a number.
std::wstring Effective(const Skin &skin, const Values &values, const Adjustment &adjustment);
bool IsAdjustmentKey(const std::wstring &key);
bool IsAdjustmentValue(const std::wstring &key, const std::wstring &value);
// Whether `value` may be given for `key` in this skin: one of its settings with a value that
// setting can take, or an adjustment of an element the skin has with a value its attribute
// takes.
bool Accepts(const Skin &skin, const std::wstring &key, const std::wstring &value);
// The adjustments of attributes that take their value from `setting`.
std::vector<std::wstring> AdjustmentsOf(const Skin &skin, const std::wstring &setting);

// --- the skin with its settings applied --------------------------------------------------

enum class Align { Auto, Left, Center, Right };
// How a panel comes in once the logon screen's wallpaper is up: fading in, rising a little while
// it fades, growing a little while it fades, or at once.
enum class Enter { Fade, Rise, Zoom, None };
enum class Case { None, Upper, Lower };
// What is drawn behind a panel: a soft glow that fades out around the ink, or a plate with
// rounded corners around it.
enum class Shape { Glow, Plate };

// How a spectrum draws its bands: as bars, as a line through their tops, or as the area under
// that line.
enum class Mode { Bars, Line, Area };
// Which way the bands reach from where they start: up from the bottom, down from the top, or
// both ways from the middle.
enum class Direction { Up, Down, Both };
// Where the bass is: at the left end, at the right, or in the middle with the treble at both ends.
enum class Bass { Left, Right, Middle };
// How a spectrum's colours lie: from its left end to its right, from where the bands start to where
// they reach, or each bar in the one colour its height reads off the gradient (a line or an area,
// which has no bars, as Vertical).
enum class Gradient { Horizontal, Vertical, Level };

// A colour at a place along a gradient, `at` from 0 to 1.
struct Stop {
    float at = 0.0f;
    uint32_t color = 0xFFFFFFFF;  // 0xAARRGGBB
};
// "#RRGGBB[AA] P%, ...": one to eight colours, each at a place from 0% to 100%, the places in
// order. The value of a Colors setting and of a spectrum's `colors`.
bool ParseStops(const std::wstring &text, std::vector<Stop> *out);

// A <spectrum>'s own attributes. Its length, corner radius, opacity and the space before it are a
// Text's, as a bar's are.
struct Spectrum {
    int bands = 64;
    float low = 40.0f, high = 16000.0f;  // Hz
    float sensitivity = 1.0f, smoothing = 0.5f;
    Mode mode = Mode::Bars;
    Direction direction = Direction::Up;
    Bass bass = Bass::Left;
    float height = 1.0f;   // em, as far as a band reaches
    float gap = 0.35f;     // between bars, as a share of each band's part of the length
    float floor = 0.05f;   // a band at rest, as a share of the height
    float stroke = 0.06f;  // em: the width of a line
    std::vector<Stop> stops{{0.0f, 0xFFFFFFFF}};  // its colours; one is a plain colour
    Gradient gradient = Gradient::Horizontal;
    float flow = 0.0f;  // seconds for the colours to slide once along the gradient and back; 0 still
    float idleOpacity = 1.0f;  // while nothing is heard, times the opacity
    float peak = 0.0f;         // the opacity of a mark held above each bar, times the opacity
};

// A text, a bar or a spectrum: what a line holds, in order.
struct Text {
    std::wstring name;   // t1, r1, a1 ...: where it is among the line's texts, bars or spectra
    std::optional<data::Condition> when;
    bool shown = true;   // false: left out entirely, as if the line did not hold it
    std::wstring value;  // may name data: {time}, {date} ...; a bar's is one fraction
    std::wstring font;   // empty: the default
    float size = 1.0f;   // em
    int weight = 400;
    bool italic = false;
    uint32_t color = 0xFFFFFFFF;  // 0xAARRGGBB
    float opacity = 1.0f;
    float tracking = 0.0f, space = 0.0f;  // em
    Case textCase = Case::None;
    bool hang = false;  // left out when the line is aligned
    float rise = 0.0f;  // em: how far above the line's baseline it sits; negative is below
    float max = 0.0f;   // em: wider words are cut short with an ellipsis; 0 is no limit
    bool tabular = false;  // every figure as wide as the others, so numbers keep their width

    bool bar = false;  // the rest is a bar's
    float length = 6.0f, thickness = 0.12f;  // em
    float radius = 0.5f;                     // of the thickness: 0.5 rounds the ends fully
    uint32_t track = 0xFFFFFFFF;             // the part not yet filled
    float trackOpacity = 0.25f;

    bool spectrum = false;  // a spectrum: `audio`, and length, radius, opacity and space
    Spectrum audio;
};

struct Line {
    float gap = 0.2f;  // em, from the line above's baseline to the top of this line's ink
    std::optional<data::Condition> when;
    bool shown = true;
    std::vector<Text> texts;
};

struct Shadow {
    float blur = 0.1f, opacity = 0.3f, x = 0.0f, y = 0.0f;  // em
    uint32_t color = 0xFF000000;
};

struct Panel {
    int anchor = 1;        // 0..8, row by row from the top left
    float size = 10.0f;    // 1 em, in percent of the display's height
    float margin = 7.0f;   // percent of the display's shorter side
    float offsetX = 0.0f, offsetY = 0.0f;  // percent of the display's width and height
    Align align = Align::Auto;
    std::vector<Line> lines;
    std::vector<Shadow> shadows;
    float backdrop = 0.0f;  // opacity of what is behind the panel's ink
    uint32_t backdropColor = 0xFF000000;
    Shape backdropShape = Shape::Glow;
    float backdropRadius = 0.35f;                               // em, a plate's corners
    float backdropPaddingX = 0.6f, backdropPaddingY = 0.4f;     // em, from the ink to a plate's edge
    float opacity = 1.0f;  // of everything the panel draws
    std::optional<data::Condition> when;
    data::Date until;  // what until.* counts down to
    Enter enter = Enter::Fade;
    float enterDelay = 0.0f;     // seconds after the wallpaper is up
    float enterDuration = 0.4f;  // seconds
};

struct Resolved {
    std::vector<Panel> panels;
};

Resolved Resolve(const Skin &skin, const Values &values);
// Whether a panel holds a spectrum, so the logon screen has to listen to its output device.
bool Listens(const Resolved &skin);

// The data a text may name.
bool IsDataName(const std::wstring &name);
// The sources of the data the component shows or tests, besides the clock, in data::Source order.
std::vector<data::Source> SourcesOf(const Skin &skin);

}  // namespace animelogon::skin
