// Skins: what is drawn over the video, declared in a small XML file. A skin is structure
// (elements and their attributes) plus settings the person can adjust; the settings' values
// live in settings.ini. Nothing in a skin computes: an attribute is either a literal, or
// "$id" for the value of one of the skin's settings, and text may name data such as {time}.
//
// A skin's structure can put any words on the screen, so it is only read from where
// administrators alone can write. Setting values carry no free text: a colour, a number in
// range, a font, or one of the skin's own options.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "animelogon/xml.h"

namespace animelogon::skin {

enum class SettingKind { Choice, Toggle, Number, Color, Font };

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
// The skin as it is stored: parsed and written again, so nothing unchecked survives.
std::string Normalize(const Skin &skin);
// The skin that comes with AnimeLogon.
const Skin &Default();
std::string DefaultText();

bool IsValue(const Setting &setting, const std::wstring &value);
std::wstring ValueOf(const Setting &setting, const Values &values);
bool IsSettingId(const std::wstring &id);
bool IsSkinId(const std::wstring &id);  // "default" or 16 lowercase hex digits

// --- the skin with its settings applied --------------------------------------------------

enum class Align { Auto, Left, Center, Right };
enum class Hours { Auto, H12, H24 };
enum class Case { None, Upper, Lower };

struct Text {
    std::wstring value;  // may name data: {time}, {date} ...
    std::wstring font;   // empty: the default
    float size = 1.0f;   // em
    int weight = 400;
    bool italic = false;
    uint32_t color = 0xFFFFFFFF;  // 0xAARRGGBB
    float opacity = 1.0f;
    float tracking = 0.0f, space = 0.0f;  // em
    Case textCase = Case::None;
    bool hang = false;  // left out when the line is aligned
};

struct Line {
    float gap = 0.2f;  // em, from the line above's baseline to the top of this line's ink
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
    std::wstring locale;   // empty: the regional format
    Hours hours = Hours::Auto;
    std::vector<Line> lines;
    std::vector<Shadow> shadows;
    float backdrop = 0.0f;  // opacity of a soft dark glow behind the panel
    uint32_t backdropColor = 0xFF000000;
};

struct Resolved {
    std::vector<Panel> panels;
};

Resolved Resolve(const Skin &skin, const Values &values);

// The data a text may name.
bool IsDataName(const std::wstring &name);

}  // namespace animelogon::skin
