// The component format: what is drawn over the wallpaper, declared in a small XML file whose
// root element is <component>. The code calls a component a skin. A component is structure
// (elements and their attributes) plus settings the person can adjust; the settings' values
// come from a theme and from settings.ini. Nothing in a component computes: an attribute is either a literal, or "$id"
// for the value of one of its settings, and text may name data such as {time} or
// {battery.percent} (data.h). A bar shows a fraction such as {media.progress}, and a panel,
// line, text or bar may be drawn only `when` a condition on the data holds. Beyond the
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

enum class SettingKind { Choice, Toggle, Number, Color, Font, Date };

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
// second line, p1.l2.r1 the first bar on it, p1.s1 its first shadow, p1.b its backdrop. An adjustment is stored under the
// element's name and the attribute's, as in p1.l1.t1.weight, and wins over the skin's own
// value for that attribute.

enum class Control { Slider, Choice, Color, Font };

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
enum class Case { None, Upper, Lower };

// A text, or a bar: what a line holds, in order.
struct Text {
    std::wstring name;   // t1, r1 ...: where it is among the line's texts or its bars
    std::optional<data::Condition> when;
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

    bool bar = false;  // the rest is a bar's
    float length = 6.0f, thickness = 0.12f;  // em
    float radius = 0.5f;                     // of the thickness: 0.5 rounds the ends fully
    uint32_t track = 0xFFFFFFFF;             // the part not yet filled
    float trackOpacity = 0.25f;
};

struct Line {
    float gap = 0.2f;  // em, from the line above's baseline to the top of this line's ink
    std::optional<data::Condition> when;
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
    float backdrop = 0.0f;  // opacity of a soft dark glow behind the panel
    uint32_t backdropColor = 0xFF000000;
    std::optional<data::Condition> when;
    data::Date until;  // what until.* counts down to
};

struct Resolved {
    std::vector<Panel> panels;
};

Resolved Resolve(const Skin &skin, const Values &values);

// The data a text may name.
bool IsDataName(const std::wstring &name);
// The sources of the data the component shows or tests, besides the clock, in data::Source order.
std::vector<data::Source> SourcesOf(const Skin &skin);

}  // namespace animelogon::skin
