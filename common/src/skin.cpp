#include "animelogon/skin.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <set>

#include "animelogon/clock.h"
#include "animelogon/text.h"

namespace animelogon::skin {
namespace {

constexpr size_t kMaxSettings = 32, kMaxOptions = 32, kMaxPanels = 8, kMaxLines = 8, kMaxTexts = 8;
constexpr size_t kMaxShadows = 4, kMaxLabel = 40, kMaxDetail = 80, kMaxTemplate = 256;

const wchar_t *const kAnchors[] = {L"top-left", L"top", L"top-right", L"left", L"center",
                                   L"right", L"bottom-left", L"bottom", L"bottom-right"};
enum class Type { Label, Anchor, Number, Align, Template, Font, Weight, Style, Color, Case, Bool, Condition, Date, Fraction, Enter,
                  Count, Mode, Direction, Bass, Gradient };

// `adjustable` attributes can be set by hand in the settings app.
struct AttributeSpec {
    const wchar_t *name;
    Type type;
    double min = 0, max = 0;
    bool adjustable = true;
};

const AttributeSpec kPanel[] = {{L"label", Type::Label, 0, 0, false}, {L"anchor", Type::Anchor},
                                {L"size", Type::Number, 1, 50},       {L"margin", Type::Number, 0, 40},
                                {L"offset-x", Type::Number, -50, 50}, {L"offset-y", Type::Number, -50, 50},
                                {L"align", Type::Align},              {L"when", Type::Condition, 0, 0, false},
                                {L"until", Type::Date, 0, 0, false}, {L"enter", Type::Enter},
                                {L"enter-delay", Type::Number, 0, 10}, {L"enter-duration", Type::Number, 0, 5}};
const AttributeSpec kLine[] = {{L"label", Type::Label, 0, 0, false}, {L"gap", Type::Number, -2, 4},
                               {L"when", Type::Condition, 0, 0, false}};
const AttributeSpec kText[] = {{L"label", Type::Label, 0, 0, false}, {L"value", Type::Template, 0, 0, false},
                               {L"when", Type::Condition, 0, 0, false},
                               {L"font", Type::Font},                {L"size", Type::Number, 0.05, 4},
                               {L"weight", Type::Weight},            {L"style", Type::Style},
                               {L"color", Type::Color},              {L"opacity", Type::Number, 0, 1},
                               {L"tracking", Type::Number, -0.5, 1}, {L"space", Type::Number, 0, 4},
                               {L"case", Type::Case},                {L"hang", Type::Bool}};
const AttributeSpec kShadow[] = {{L"label", Type::Label, 0, 0, false},
                                 {L"blur", Type::Number, 0, 1},
                                 {L"opacity", Type::Number, 0, 1},
                                 {L"x", Type::Number, -1, 1},
                                 {L"y", Type::Number, -1, 1},
                                 {L"color", Type::Color}};
const AttributeSpec kBackdrop[] = {
    {L"label", Type::Label, 0, 0, false}, {L"opacity", Type::Number, 0, 1}, {L"color", Type::Color}};
const AttributeSpec kBar[] = {{L"label", Type::Label, 0, 0, false}, {L"value", Type::Fraction, 0, 0, false},
                              {L"when", Type::Condition, 0, 0, false}, {L"length", Type::Number, 0.2, 40},
                              {L"thickness", Type::Number, 0.02, 1},   {L"radius", Type::Number, 0, 0.5},
                              {L"color", Type::Color},                 {L"opacity", Type::Number, 0, 1},
                              {L"track", Type::Color},                 {L"track-opacity", Type::Number, 0, 1},
                              {L"space", Type::Number, 0, 4}};
const AttributeSpec kSpectrum[] = {
    {L"label", Type::Label, 0, 0, false},   {L"when", Type::Condition, 0, 0, false},
    {L"mode", Type::Mode},                  {L"direction", Type::Direction},
    {L"bass", Type::Bass},                  {L"length", Type::Number, 0.5, 80},
    {L"height", Type::Number, 0.05, 8},     {L"bands", Type::Count, 4, 128},
    {L"gap", Type::Number, 0, 0.95},        {L"radius", Type::Number, 0, 0.5},
    {L"stroke", Type::Number, 0.005, 1},    {L"floor", Type::Number, 0, 1},
    {L"color", Type::Color},                {L"end-color", Type::Color},
    {L"gradient", Type::Gradient},          {L"opacity", Type::Number, 0, 1},
    {L"idle-opacity", Type::Number, 0, 1},  {L"peak", Type::Number, 0, 1},
    {L"sensitivity", Type::Number, 0.05, 10}, {L"smoothing", Type::Number, 0, 0.99},
    {L"low", Type::Number, 10, 20000},      {L"high", Type::Number, 20, 24000},
    {L"space", Type::Number, 0, 4}};

struct SpecList {
    const AttributeSpec *begin;
    size_t size;
};
template <size_t N>
constexpr SpecList List(const AttributeSpec (&specs)[N]) {
    return {specs, N};
}
// The attributes an element may carry, by its name.
SpecList SpecsOf(const std::wstring &element) {
    if (element == L"panel") return List(kPanel);
    if (element == L"line") return List(kLine);
    if (element == L"text") return List(kText);
    if (element == L"shadow") return List(kShadow);
    if (element == L"backdrop") return List(kBackdrop);
    if (element == L"bar") return List(kBar);
    if (element == L"spectrum") return List(kSpectrum);
    return {nullptr, 0};
}
const AttributeSpec *SpecFor(const std::wstring &element, const std::wstring &attribute) {
    const SpecList list = SpecsOf(element);
    for (size_t i = 0; i < list.size; ++i)
        if (attribute == list.begin[i].name) return &list.begin[i];
    return nullptr;
}

template <size_t N>
int IndexOf(const wchar_t *const (&names)[N], const std::wstring &value) {
    for (size_t i = 0; i < N; ++i)
        if (value == names[i]) return (int)i;
    return -1;
}

bool ParseNumber(const std::wstring &text, double *out) {
    if (text.empty() || text.size() > 24) return false;
    wchar_t *end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

int WeightOf(const std::wstring &text) {
    static const struct {
        const wchar_t *name;
        int weight;
    } names[] = {{L"thin", 100},     {L"extralight", 200}, {L"light", 300}, {L"regular", 400},
                 {L"medium", 500},   {L"semibold", 600},   {L"bold", 700},  {L"extrabold", 800},
                 {L"black", 900}};
    for (const auto &n : names)
        if (text == n.name) return n.weight;
    double v = 0;
    return ParseNumber(text, &v) && v >= 1 && v <= 999 && v == std::floor(v) ? (int)v : 0;
}

// #RRGGBB or #RRGGBBAA, as 0xAARRGGBB.
bool ParseRgba(const std::wstring &text, uint32_t *out) {
    uint32_t rgb = 0;
    if (text.size() == 7 && ParseColor(text, &rgb)) return *out = 0xFF000000u | rgb, true;
    if (text.size() != 9 || !ParseColor(text.substr(0, 7), &rgb)) return false;
    uint32_t alpha = 0;
    if (!ParseColor(L"#0000" + text.substr(7), &alpha)) return false;
    *out = (alpha << 24) | rgb;
    return true;
}

bool IsTemplate(const std::wstring &text) {
    if (text.size() > kMaxTemplate) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'}') return false;
        if (text[i] != L'{') continue;
        const size_t end = text.find(L'}', i);
        if (end == std::wstring::npos || !IsDataName(text.substr(i + 1, end - i - 1))) return false;
        i = end;
    }
    return true;
}

// "{name}" for a variable that is a fraction.
bool IsFraction(const std::wstring &text) {
    if (text.size() < 3 || text.front() != L'{' || text.back() != L'}') return false;
    const data::Variable *v = data::Find(text.substr(1, text.size() - 2));
    return v && v->kind == data::Kind::Fraction;
}

bool IsLabel(const std::wstring &text, size_t limit = kMaxLabel) {
    if (text.empty() || text.size() > limit) return false;
    for (wchar_t c : text)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool Check(const AttributeSpec &spec, const std::wstring &value) {
    double v = 0;
    switch (spec.type) {
    case Type::Label: return IsLabel(value);
    case Type::Anchor: return IndexOf(kAnchors, value) >= 0;
    case Type::Number: return ParseNumber(value, &v) && v >= spec.min && v <= spec.max;
    case Type::Align: return value == L"auto" || value == L"left" || value == L"center" || value == L"right";
    case Type::Template: return IsTemplate(value);
    case Type::Font: return value.empty() || IsFontFamilyName(value);
    case Type::Weight: return WeightOf(value) > 0;
    case Type::Style: return value == L"normal" || value == L"italic";
    case Type::Color: {
        uint32_t c = 0;
        return ParseRgba(value, &c);
    }
    case Type::Case: return value == L"none" || value == L"upper" || value == L"lower";
    case Type::Bool: return value == L"true" || value == L"false";
    case Type::Condition: {
        data::Condition c;
        return data::ParseCondition(value, &c);
    }
    case Type::Date: {
        data::Date d;
        return data::ParseDate(value, &d);
    }
    case Type::Fraction: return IsFraction(value);
    case Type::Enter: return value == L"fade" || value == L"rise" || value == L"zoom" || value == L"none";
    case Type::Count: return ParseNumber(value, &v) && v == std::floor(v) && v >= spec.min && v <= spec.max;
    case Type::Mode: return value == L"bars" || value == L"line" || value == L"area";
    case Type::Direction: return value == L"up" || value == L"down" || value == L"both";
    case Type::Bass: return value == L"left" || value == L"right" || value == L"middle";
    case Type::Gradient: return value == L"none" || value == L"horizontal" || value == L"vertical";
    }
    return false;
}

// Whether every value `setting` can take suits the attribute.
bool Suits(const Setting &setting, const AttributeSpec &spec) {
    switch (setting.kind) {
    case SettingKind::Choice:
    case SettingKind::Toggle:
        for (const Option &o : setting.options)
            if (!Check(spec, o.value)) return false;
        return true;
    case SettingKind::Number:
        // A count takes whole numbers only, so the setting must step by them from one.
        if (spec.type == Type::Count)
            return setting.min >= spec.min && setting.max <= spec.max && setting.min == std::floor(setting.min) &&
                   setting.step == std::floor(setting.step);
        return spec.type == Type::Number && setting.min >= spec.min && setting.max <= spec.max;
    case SettingKind::Color: return spec.type == Type::Color;
    case SettingKind::Font: return spec.type == Type::Font;
    case SettingKind::Date: return spec.type == Type::Date;
    }
    return false;
}

class Checker {
public:
    explicit Checker(Skin *skin) : skin_(skin) {}

    bool Run() {
        const xml::Element &root = skin_->document;
        if (root.name != L"component") return Fail(root, L"the root element must be <component>");
        for (const auto &[name, value] : root.attributes) {
            if (name == L"format") {
                if (value != L"1") return Fail(root, L"unsupported format " + value);
            } else if (name == L"name" || name == L"author") {
                if (!IsLabel(value) && !(name == L"author" && value.empty()))
                    return Fail(root, L"a bad " + name);
                (name == L"name" ? skin_->name : skin_->author) = value;
            } else {
                return Fail(root, L"unknown attribute " + name);
            }
        }
        if (!root.Find(L"format") || skin_->name.empty()) return Fail(root, L"<" + root.name + L"> needs format and name");
        size_t panels = 0;
        bool settingsSeen = false;
        for (const xml::Element &e : root.children) {
            if (e.name == L"settings") {
                if (settingsSeen || panels) return Fail(e, L"<settings> must come once, before the panels");
                settingsSeen = true;
                if (!Settings(e)) return false;
            } else if (e.name == L"panel") {
                if (++panels > kMaxPanels) return Fail(e, L"too many panels");
                if (!Panel(e)) return false;
            } else {
                return Fail(e, L"unknown element <" + e.name + L">");
            }
        }
        if (!panels) return Fail(root, L"a skin needs a <panel>");
        return true;
    }

    const std::wstring &error() const { return error_; }

private:
    bool Fail(const xml::Element &e, const std::wstring &what) {
        if (error_.empty()) error_ = Format(L"line %d: %s", e.line, what.c_str());
        return false;
    }

    bool Settings(const xml::Element &list) {
        if (!list.attributes.empty()) return Fail(list, L"<settings> takes no attributes");
        for (const xml::Element &e : list.children) {
            if (skin_->settings.size() >= kMaxSettings) return Fail(e, L"too many settings");
            Setting s;
            if (e.name == L"choice") s.kind = SettingKind::Choice;
            else if (e.name == L"toggle") s.kind = SettingKind::Toggle;
            else if (e.name == L"number") s.kind = SettingKind::Number;
            else if (e.name == L"color") s.kind = SettingKind::Color;
            else if (e.name == L"font") s.kind = SettingKind::Font;
            else if (e.name == L"date") s.kind = SettingKind::Date;
            else return Fail(e, L"unknown setting <" + e.name + L">");
            const std::wstring *on = nullptr, *off = nullptr, *min = nullptr, *max = nullptr, *step = nullptr;
            bool hasDefault = false;
            for (const auto &[name, value] : e.attributes) {
                if (name == L"id") s.id = value;
                else if (name == L"label") s.label = value;
                else if (name == L"detail") s.detail = value;
                else if (name == L"default") s.fallback = value, hasDefault = true;
                else if (name == L"on" && s.kind == SettingKind::Toggle) on = &value;
                else if (name == L"off" && s.kind == SettingKind::Toggle) off = &value;
                else if (name == L"min" && s.kind == SettingKind::Number) min = &value;
                else if (name == L"max" && s.kind == SettingKind::Number) max = &value;
                else if (name == L"step" && s.kind == SettingKind::Number) step = &value;
                else return Fail(e, L"unknown attribute " + name);
            }
            if (!IsSettingId(s.id)) return Fail(e, L"a bad setting id");
            // settings.ini keeps a theme's edits to one of its components under
            // theme.<theme>.<instance>.<key>, where "ref" and "visible" are the instance's own.
            if (s.id == L"ref" || s.id == L"visible") return Fail(e, L"setting id " + s.id + L" is reserved");
            if (skin_->Find(s.id)) return Fail(e, L"setting " + s.id + L" is repeated");
            if (!IsLabel(s.label)) return Fail(e, L"setting " + s.id + L" needs a label");
            if (!s.detail.empty() && !IsLabel(s.detail, kMaxDetail)) return Fail(e, L"setting " + s.id + L" has a bad detail");
            if (s.kind == SettingKind::Toggle) {
                if (!on || !off) return Fail(e, L"a toggle needs on and off");
                s.options = {{*on, L"on"}, {*off, L"off"}};
            } else if (s.kind == SettingKind::Number) {
                if (!min || !max || !ParseNumber(*min, &s.min) || !ParseNumber(*max, &s.max) || s.min >= s.max ||
                    (step && (!ParseNumber(*step, &s.step) || s.step <= 0)))
                    return Fail(e, L"a number needs min < max and a positive step");
            } else if (s.kind == SettingKind::Choice) {
                for (const xml::Element &o : e.children) {
                    const std::wstring *value = o.Find(L"value"), *label = o.Find(L"label");
                    if (o.name != L"option" || !value || !label || o.attributes.size() != 2 || !o.children.empty() ||
                        !IsLabel(*label))
                        return Fail(o, L"an option needs a value and a label");
                    if (s.options.size() >= kMaxOptions) return Fail(o, L"too many options");
                    s.options.push_back({*value, *label});
                }
                if (s.options.empty()) return Fail(e, L"a choice needs options");
            }
            if (s.kind != SettingKind::Choice && !e.children.empty()) return Fail(e, L"unexpected children");
            if (!hasDefault || !IsValue(s, s.fallback)) return Fail(e, L"setting " + s.id + L" has a bad default");
            skin_->settings.push_back(std::move(s));
        }
        return true;
    }

    template <size_t N>
    bool Attributes(const xml::Element &e, const AttributeSpec (&specs)[N]) {
        for (const auto &[name, value] : e.attributes) {
            const AttributeSpec *spec = nullptr;
            for (const AttributeSpec &s : specs)
                if (name == s.name) spec = &s;
            if (!spec) return Fail(e, L"unknown attribute " + name + L" on <" + e.name + L">");
            if (!value.empty() && value[0] == L'$' && spec->type != Type::Label) {
                const Setting *setting = skin_->Find(value.substr(1));
                if (!setting) return Fail(e, L"no setting " + value);
                if (!Suits(*setting, *spec)) return Fail(e, L"setting " + value + L" does not suit " + name);
            } else if (!Check(*spec, value)) {
                return Fail(e, L"a bad value for " + name);
            }
        }
        return true;
    }

    bool Panel(const xml::Element &panel) {
        if (!Attributes(panel, kPanel)) return false;
        size_t lines = 0, shadows = 0, backdrops = 0;
        for (const xml::Element &e : panel.children) {
            if (e.name == L"line") {
                if (++lines > kMaxLines) return Fail(e, L"too many lines");
                if (!Attributes(e, kLine)) return false;
                if (e.children.empty() || e.children.size() > kMaxTexts)
                    return Fail(e, L"a line needs 1 to 8 texts and bars");
                for (const xml::Element &t : e.children) {
                    const bool spectrum = t.name == L"spectrum";
                    if (t.name != L"text" && t.name != L"bar" && !spectrum)
                        return Fail(t, L"a line holds only <text>, <bar> and <spectrum>");
                    if (!t.children.empty()) return Fail(t, L"<" + t.name + L"> holds nothing");
                    // A spectrum shows the sound playing; it names no data.
                    if (!spectrum && !t.Find(L"value")) return Fail(t, L"<" + t.name + L"> needs a value");
                    if (spectrum ? !Attributes(t, kSpectrum) : t.name == L"text" ? !Attributes(t, kText) : !Attributes(t, kBar))
                        return false;
                }
            } else if (e.name == L"shadow" || e.name == L"backdrop") {
                if (!e.children.empty()) return Fail(e, L"<" + e.name + L"> holds nothing");
                if (e.name == L"shadow" ? ++shadows > kMaxShadows : ++backdrops > 1)
                    return Fail(e, L"too many <" + e.name + L">");
                if (e.name == L"shadow" ? !Attributes(e, kShadow) : !Attributes(e, kBackdrop)) return false;
            } else {
                return Fail(e, L"unknown element <" + e.name + L">");
            }
        }
        if (!lines) return Fail(panel, L"a panel needs a <line>");
        return true;
    }

    Skin *skin_;
    std::wstring error_;
};

// Reads attributes once their settings are applied.
class Applied {
public:
    Applied(const Skin &skin, const Values &values, const xml::Element &e, std::wstring key)
        : skin_(skin), values_(values), e_(e), key_(std::move(key)) {}

    const std::wstring *Raw(const wchar_t *name) {
        const auto adjusted = values_.find(key_ + L"." + name);
        if (adjusted != values_.end()) {
            const AttributeSpec *spec = SpecFor(e_.name, name);
            if (spec && spec->adjustable && Check(*spec, adjusted->second)) return &adjusted->second;
        }
        const std::wstring *v = e_.Find(name);
        if (!v || v->empty() || (*v)[0] != L'$') return v;
        const Setting *s = skin_.Find(v->substr(1));
        if (!s) return nullptr;
        resolved_ = ValueOf(*s, values_);
        if (s->kind == SettingKind::Toggle) resolved_ = s->options[resolved_ == L"on" ? 0 : 1].value;
        return &resolved_;
    }
    std::wstring String(const wchar_t *name, const std::wstring &fallback = {}) {
        const std::wstring *v = Raw(name);
        return v ? *v : fallback;
    }
    float Number(const wchar_t *name, float fallback) {
        double v = 0;
        const std::wstring *raw = Raw(name);
        return raw && ParseNumber(*raw, &v) ? (float)v : fallback;
    }
    uint32_t Color(const wchar_t *name, uint32_t fallback) {
        uint32_t c = 0;
        const std::wstring *raw = Raw(name);
        return raw && ParseRgba(*raw, &c) ? c : fallback;
    }

private:
    const Skin &skin_;
    const Values &values_;
    const xml::Element &e_;
    std::wstring key_;
    std::wstring resolved_;
};

// --- adjustments -------------------------------------------------------------------------

struct Offer {
    const wchar_t *element, *attribute, *label, *detail;
    Control control;
    double min, max, step;
    const wchar_t *fallback;  // when the element does not say
};

// What the settings app offers for each element, in order. Slider ranges are what is useful
// to drag through; a skin may still write any value its attribute allows.
const Offer kOffers[] = {
    {L"panel", L"anchor", L"锚点", L"组件在屏幕上的对齐位置。", Control::Choice, 0, 0, 0, L"top"},
    {L"panel", L"offset-x", L"水平偏移", L"以屏幕宽度的百分比计，正值向右。", Control::Slider, -50, 50, 0.1, L"0"},
    {L"panel", L"offset-y", L"垂直偏移", L"以屏幕高度的百分比计，正值向下。", Control::Slider, -50, 50, 0.1, L"0"},
    {L"panel", L"margin", L"边距", L"与屏幕边缘的距离，以屏幕短边的百分比计。", Control::Slider, 0, 40, 0.5, L"7"},
    {L"panel", L"size", L"大小", L"以屏幕高度的百分比计，其他文字按比例缩放。", Control::Slider, 1, 50, 0.5, L"10"},
    {L"panel", L"align", L"文字对齐", L"多行文字的对齐方式。", Control::Choice, 0, 0, 0, L"auto"},
    {L"panel", L"enter", L"效果", L"锁屏显示时组件的入场效果。", Control::Choice, 0, 0, 0, L"fade"},
    {L"panel", L"enter-delay", L"延迟", L"壁纸开始显示后，延迟多久开始入场。", Control::Slider, 0, 5, 0.05, L"0"},
    {L"panel", L"enter-duration", L"时长", L"入场动画的持续时间。", Control::Slider, 0, 3, 0.05, L"0.4"},
    {L"text", L"font", L"字体", L"仅列出为所有用户安装的字体。", Control::Font, 0, 0, 0, L""},
    {L"text", L"weight", L"字重", L"字体不含所选字重时，使用最接近的字重。", Control::Slider, 100, 900, 50, L"400"},
    {L"text", L"size", L"相对大小", L"相对于组件大小。", Control::Slider, 0.05, 2, 0.01, L"1"},
    {L"text", L"tracking", L"字距", L"相对于本段文字的字号。", Control::Slider, -0.2, 0.5, 0.005, L"0"},
    {L"text", L"space", L"前间距", L"与前一段文字的间距，相对于组件大小。", Control::Slider, 0, 2, 0.01, L"0"},
    {L"line", L"gap", L"行距", L"到上一行基线的距离，相对于组件大小。", Control::Slider, -1, 2, 0.01, L"0.2"},
    {L"text", L"color", L"颜色", L"", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"text", L"opacity", L"不透明度", L"", Control::Slider, 0, 1, 0.01, L"1"},
    {L"text", L"style", L"字形", L"", Control::Choice, 0, 0, 0, L"normal"},
    {L"text", L"case", L"大小写", L"仅作用于拉丁字母。", Control::Choice, 0, 0, 0, L"none"},
    // Offered only where the component lets a text hang (Parts).
    {L"text", L"hang", L"行内对齐", L"悬挂时，该文字不参与各行之间的对齐，对齐以其余文字为准。", Control::Choice, 0, 0, 0, L"false"},
    {L"shadow", L"opacity", L"不透明度", L"为 0 时不显示阴影。", Control::Slider, 0, 1, 0.01, L"0.3"},
    {L"shadow", L"blur", L"模糊", L"相对于组件大小。", Control::Slider, 0, 0.5, 0.005, L"0.1"},
    {L"shadow", L"x", L"水平偏移", L"相对于组件大小。", Control::Slider, -0.3, 0.3, 0.005, L"0"},
    {L"shadow", L"y", L"垂直偏移", L"相对于组件大小。", Control::Slider, -0.3, 0.3, 0.005, L"0"},
    {L"shadow", L"color", L"颜色", L"", Control::Color, 0, 0, 0, L"#000000"},
    {L"backdrop", L"opacity", L"不透明度", L"在文字下方加深背景以提高可读性；为 0 时不显示。", Control::Slider, 0, 1, 0.01, L"0"},
    {L"backdrop", L"color", L"颜色", L"", Control::Color, 0, 0, 0, L"#000000"},
    {L"bar", L"length", L"长度", L"相对于组件大小。", Control::Slider, 0.5, 20, 0.1, L"6"},
    {L"bar", L"thickness", L"粗细", L"相对于组件大小。", Control::Slider, 0.02, 0.5, 0.005, L"0.12"},
    {L"bar", L"radius", L"圆角", L"0 为直角，0.5 为半圆形端点。", Control::Slider, 0, 0.5, 0.01, L"0.5"},
    {L"bar", L"space", L"前间距", L"与前一段内容的间距，相对于组件大小。", Control::Slider, 0, 2, 0.01, L"0"},
    {L"bar", L"color", L"颜色", L"已完成部分。", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"bar", L"opacity", L"不透明度", L"", Control::Slider, 0, 1, 0.01, L"1"},
    {L"bar", L"track", L"轨道颜色", L"未完成部分。", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"bar", L"track-opacity", L"轨道不透明度", L"为 0 时不显示轨道。", Control::Slider, 0, 1, 0.01, L"0.25"},
    {L"spectrum", L"mode", L"样式", L"柱形分别显示各频段；曲线连接各频段的顶端，填充曲线同时填充其下方的区域。", Control::Choice, 0, 0, 0, L"bars"},
    {L"spectrum", L"direction", L"方向", L"频段自基线伸展的方向；双向时自中线向两侧伸展。", Control::Choice, 0, 0, 0, L"up"},
    {L"spectrum", L"bass", L"低频位置", L"低频位于中间时，频谱左右对称。", Control::Choice, 0, 0, 0, L"left"},
    {L"spectrum", L"length", L"宽度", L"相对于组件大小。", Control::Slider, 1, 40, 0.1, L"12"},
    {L"spectrum", L"height", L"高度", L"频段可达到的最大高度，相对于组件大小。", Control::Slider, 0.1, 4, 0.01, L"1"},
    {L"spectrum", L"bands", L"频段数量", L"", Control::Slider, 4, 128, 1, L"64"},
    {L"spectrum", L"gap", L"间隔", L"相邻柱形之间的间隔，以每个频段所占宽度的比例计。", Control::Slider, 0, 0.9, 0.01, L"0.35"},
    {L"spectrum", L"radius", L"圆角", L"0 为直角，0.5 为半圆形端点。", Control::Slider, 0, 0.5, 0.01, L"0.5"},
    {L"spectrum", L"stroke", L"线宽", L"曲线的粗细，相对于组件大小。", Control::Slider, 0.01, 0.3, 0.005, L"0.06"},
    {L"spectrum", L"floor", L"最小高度", L"无声音时各频段的高度，以频谱高度的比例计。", Control::Slider, 0, 0.5, 0.005, L"0.05"},
    {L"spectrum", L"color", L"起始颜色", L"渐变方向为「无」时，仅使用此颜色。", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"spectrum", L"end-color", L"结束颜色", L"", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"spectrum", L"gradient", L"渐变方向", L"水平渐变自左向右；垂直渐变自基线向频段顶端。", Control::Choice, 0, 0, 0, L"none"},
    {L"spectrum", L"opacity", L"不透明度", L"", Control::Slider, 0, 1, 0.01, L"1"},
    {L"spectrum", L"idle-opacity", L"无声音时的不透明度", L"相对于不透明度；为 0 时，无声音时不显示频谱。", Control::Slider, 0, 1, 0.01, L"1"},
    {L"spectrum", L"peak", L"峰值标记", L"柱形上方短暂停留后回落的标记的不透明度；为 0 时不显示。", Control::Slider, 0, 1, 0.01, L"0"},
    {L"spectrum", L"sensitivity", L"灵敏度", L"数值越大，较轻的声音也会使频段升得越高。", Control::Slider, 0.1, 5, 0.05, L"1"},
    {L"spectrum", L"smoothing", L"平滑度", L"数值越大，频段的升降越平缓。", Control::Slider, 0, 0.95, 0.01, L"0.5"},
    {L"spectrum", L"low", L"最低频率", L"以赫兹计。", Control::Slider, 20, 1000, 5, L"40"},
    {L"spectrum", L"high", L"最高频率", L"以赫兹计。", Control::Slider, 1000, 20000, 100, L"16000"},
    {L"spectrum", L"space", L"前间距", L"与前一段内容的间距，相对于组件大小。", Control::Slider, 0, 2, 0.01, L"0"},
};

std::vector<Option> OptionsOf(const std::wstring &attribute) {
    if (attribute == L"anchor")
        return {{L"top-left", L"左上"},    {L"top", L"上方居中"},       {L"top-right", L"右上"},
                {L"left", L"左侧居中"},    {L"center", L"居中"},        {L"right", L"右侧居中"},
                {L"bottom-left", L"左下"}, {L"bottom", L"下方居中"},    {L"bottom-right", L"右下"}};
    if (attribute == L"align")
        return {{L"auto", L"自动"}, {L"left", L"左对齐"}, {L"center", L"居中"}, {L"right", L"右对齐"}};
    if (attribute == L"style") return {{L"normal", L"常规"}, {L"italic", L"斜体"}};
    if (attribute == L"case") return {{L"none", L"不转换"}, {L"upper", L"全部大写"}, {L"lower", L"全部小写"}};
    if (attribute == L"hang") return {{L"true", L"悬挂"}, {L"false", L"参与对齐"}};
    if (attribute == L"enter") return {{L"fade", L"淡入"}, {L"rise", L"上移淡入"}, {L"zoom", L"缩放淡入"}, {L"none", L"无"}};
    if (attribute == L"mode") return {{L"bars", L"柱形"}, {L"line", L"曲线"}, {L"area", L"填充曲线"}};
    if (attribute == L"direction") return {{L"up", L"向上"}, {L"down", L"向下"}, {L"both", L"双向"}};
    if (attribute == L"bass") return {{L"left", L"左侧"}, {L"middle", L"中间"}, {L"right", L"右侧"}};
    if (attribute == L"gradient") return {{L"none", L"无"}, {L"horizontal", L"水平"}, {L"vertical", L"垂直"}};
    return {};
}

const Offer *OfferFor(const std::wstring &element, const std::wstring &attribute) {
    for (const Offer &o : kOffers)
        if (element == o.element && attribute == o.attribute) return &o;
    return nullptr;
}

// p1, p1.l2, p1.l2.t1, p1.s1, p1.b: the element's name, and the element itself.
const xml::Element *ElementAt(const Skin &skin, const std::wstring &key) {
    const xml::Element *at = &skin.document;
    size_t i = 0;
    while (i < key.size()) {
        size_t end = key.find(L'.', i);
        if (end == std::wstring::npos) end = key.size();
        const std::wstring step = key.substr(i, end - i);
        i = end + 1;
        if (step.empty()) return nullptr;
        const wchar_t kind = step[0];
        const wchar_t *name = kind == L'p' ? L"panel" : kind == L'l' ? L"line" : kind == L't' ? L"text"
                            : kind == L'r' ? L"bar" : kind == L'a' ? L"spectrum" : kind == L's' ? L"shadow"
                            : kind == L'b' ? L"backdrop" : nullptr;
        if (!name) return nullptr;
        size_t wanted = 1;
        if (kind == L'b') {
            if (step.size() != 1) return nullptr;
        } else {
            if (step.size() < 2 || step.size() > 3) return nullptr;
            wanted = 0;
            for (size_t k = 1; k < step.size(); ++k) {
                if (step[k] < L'0' || step[k] > L'9') return nullptr;
                wanted = wanted * 10 + (size_t)(step[k] - L'0');
            }
            if (!wanted) return nullptr;
        }
        const xml::Element *found = nullptr;
        size_t seen = 0;
        for (const xml::Element &c : at->children)
            if (c.name == name && ++seen == wanted) {
                found = &c;
                break;
            }
        if (!found) return nullptr;
        at = found;
    }
    return at == &skin.document ? nullptr : at;
}

// The element names an adjustment key can have, and which element each is: checked by shape
// alone, so settings.ini can be read before the skin is.
bool KeyShape(const std::wstring &key, std::wstring *element, std::wstring *attribute) {
    const size_t dot = key.rfind(L'.');
    if (dot == std::wstring::npos || key.size() > 64) return false;
    *attribute = key.substr(dot + 1);
    const std::wstring path = key.substr(0, dot);
    std::vector<std::wstring> steps;
    for (size_t i = 0; i <= path.size();) {
        size_t end = path.find(L'.', i);
        if (end == std::wstring::npos) end = path.size();
        steps.push_back(path.substr(i, end - i));
        i = end + 1;
    }
    auto numbered = [](const std::wstring &s, wchar_t kind) {
        if (s.size() < 2 || s.size() > 3 || s[0] != kind || s[1] == L'0') return false;
        for (size_t k = 1; k < s.size(); ++k)
            if (s[k] < L'0' || s[k] > L'9') return false;
        return true;
    };
    if (steps.empty() || !numbered(steps[0], L'p')) return false;
    if (steps.size() == 1) *element = L"panel";
    else if (steps.size() == 2 && numbered(steps[1], L'l')) *element = L"line";
    else if (steps.size() == 2 && numbered(steps[1], L's')) *element = L"shadow";
    else if (steps.size() == 2 && steps[1] == L"b") *element = L"backdrop";
    else if (steps.size() == 3 && numbered(steps[1], L'l') && numbered(steps[2], L't')) *element = L"text";
    else if (steps.size() == 3 && numbered(steps[1], L'l') && numbered(steps[2], L'r')) *element = L"bar";
    else if (steps.size() == 3 && numbered(steps[1], L'l') && numbered(steps[2], L'a')) *element = L"spectrum";
    else return false;
    const AttributeSpec *spec = SpecFor(*element, *attribute);
    return spec && spec->adjustable;
}

// What a text or bar shows, when it names exactly one piece of data: "{battery.percent}%" is 电量.
const wchar_t *DataLabel(const std::wstring &value) {
    const size_t open = value.find(L'{');
    if (open == std::wstring::npos || value.find(L'{', open + 1) != std::wstring::npos) return nullptr;
    const size_t close = value.find(L'}', open);
    if (close == std::wstring::npos) return nullptr;
    const data::Variable *v = data::Find(value.substr(open + 1, close - open - 1));
    return v && *v->label ? v->label : nullptr;
}

std::optional<data::Condition> ConditionOf(const xml::Element &e) {
    data::Condition c;
    const std::wstring *when = e.Find(L"when");
    if (!when || !data::ParseCondition(*when, &c)) return std::nullopt;
    return c;
}

// A <spectrum>'s attributes, its settings and adjustments applied.
void ReadSpectrum(Applied &a, Text *text) {
    Spectrum &s = text->audio;
    text->length = a.Number(L"length", 12.0f);
    text->radius = a.Number(L"radius", 0.5f);
    text->color = a.Color(L"color", text->color);
    text->opacity = a.Number(L"opacity", 1.0f);
    text->space = a.Number(L"space", 0.0f);
    s.bands = (int)std::lround(a.Number(L"bands", (float)s.bands));
    s.low = a.Number(L"low", s.low);
    s.high = a.Number(L"high", s.high);
    // The bands need some range to spread over: a high frequency at or below the low one goes an
    // octave above it.
    if (s.high < s.low * 1.5f) s.high = s.low * 2.0f;
    s.sensitivity = a.Number(L"sensitivity", s.sensitivity);
    s.smoothing = a.Number(L"smoothing", s.smoothing);
    const std::wstring mode = a.String(L"mode", L"bars"), direction = a.String(L"direction", L"up");
    const std::wstring bass = a.String(L"bass", L"left"), gradient = a.String(L"gradient", L"none");
    s.mode = mode == L"line" ? Mode::Line : mode == L"area" ? Mode::Area : Mode::Bars;
    s.direction = direction == L"down" ? Direction::Down : direction == L"both" ? Direction::Both : Direction::Up;
    s.bass = bass == L"right" ? Bass::Right : bass == L"middle" ? Bass::Middle : Bass::Left;
    s.gradient = gradient == L"horizontal" ? Gradient::Horizontal
                 : gradient == L"vertical" ? Gradient::Vertical
                                           : Gradient::None;
    s.height = a.Number(L"height", s.height);
    s.gap = a.Number(L"gap", s.gap);
    s.floor = a.Number(L"floor", s.floor);
    s.stroke = a.Number(L"stroke", s.stroke);
    s.endColor = a.Color(L"end-color", text->color);
    s.idleOpacity = a.Number(L"idle-opacity", s.idleOpacity);
    s.peak = a.Number(L"peak", s.peak);
}

}  // namespace

const Setting *Skin::Find(const std::wstring &id) const {
    for (const Setting &s : settings)
        if (s.id == id) return &s;
    return nullptr;
}

bool IsSettingId(const std::wstring &id) {
    if (id.empty() || id.size() > 32) return false;
    for (wchar_t c : id)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    return true;
}

bool IsDataName(const std::wstring &name) {
    const data::Variable *v = data::Find(name);
    return v && v->kind == data::Kind::Text;
}

std::vector<data::Source> SourcesOf(const Skin &skin) {
    std::set<data::Source> found;
    auto names = [&](const std::wstring &value) {
        for (size_t i = value.find(L'{'); i != std::wstring::npos; i = value.find(L'{', i + 1)) {
            const size_t end = value.find(L'}', i);
            if (end == std::wstring::npos) break;
            if (const data::Variable *v = data::Find(value.substr(i + 1, end - i - 1))) found.insert(v->source);
        }
    };
    std::vector<const xml::Element *> pending{&skin.document};
    while (!pending.empty()) {
        const xml::Element *e = pending.back();
        pending.pop_back();
        if (const std::wstring *v = e->Find(L"value"); v && (e->name == L"text" || e->name == L"bar")) names(*v);
        if (const auto c = ConditionOf(*e)) found.insert(data::Find(c->name)->source);
        if (e->name == L"panel" && e->Find(L"until")) found.insert(data::Source::Until);
        if (e->name == L"spectrum") found.insert(data::Source::Audio);
        for (const xml::Element &c : e->children) pending.push_back(&c);
    }
    found.erase(data::Source::Clock);
    return {found.begin(), found.end()};
}

bool IsValue(const Setting &setting, const std::wstring &value) {
    double v = 0;
    uint32_t c = 0;
    switch (setting.kind) {
    case SettingKind::Choice:
        for (const Option &o : setting.options)
            if (o.value == value) return true;
        return false;
    case SettingKind::Toggle: return value == L"on" || value == L"off";
    case SettingKind::Number: return ParseNumber(value, &v) && v >= setting.min && v <= setting.max;
    case SettingKind::Color: return ParseRgba(value, &c);
    case SettingKind::Font: return value.empty() || IsFontFamilyName(value);
    case SettingKind::Date: {
        data::Date d;
        return data::ParseDate(value, &d);
    }
    }
    return false;
}

std::wstring ValueOf(const Setting &setting, const Values &values) {
    const auto it = values.find(setting.id);
    return it != values.end() && IsValue(setting, it->second) ? it->second : setting.fallback;
}

std::vector<Part> Parts(const Skin &skin) {
    std::vector<Part> parts;
    size_t panelCount = 0;
    for (const xml::Element &p : skin.document.children) panelCount += p.name == L"panel";
    auto offer = [](Part &part, const std::wstring &element, const std::wstring &key) {
        for (const Offer &o : kOffers) {
            if (element != o.element) continue;
            Adjustment a;
            a.key = key + L"." + o.attribute;
            a.label = o.label;
            a.detail = o.detail;
            a.control = o.control;
            a.min = o.min;
            a.max = o.max;
            a.step = o.step;
            a.options = OptionsOf(o.attribute);
            part.adjustments.push_back(std::move(a));
        }
    };
    size_t panels = 0;
    for (const xml::Element &p : skin.document.children) {
        if (p.name != L"panel") continue;
        const std::wstring pk = L"p" + std::to_wstring(++panels);
        const std::wstring *label = p.Find(L"label");
        Part panel{pk, label ? *label : panelCount == 1 ? L"整体" : Format(L"面板 %zu", panels), {}};
        offer(panel, L"panel", pk);
        parts.push_back(std::move(panel));
        size_t lines = 0, shadows = 0, texts = 0, bars = 0, spectra = 0;
        for (const xml::Element &e : p.children) {
            if (e.name == L"line") {
                const std::wstring lk = pk + L".l" + std::to_wstring(++lines);
                size_t t = 0, r = 0, a = 0;
                for (const xml::Element &x : e.children) {
                    const bool isBar = x.name == L"bar", isSpectrum = x.name == L"spectrum";
                    const bool first = &x == &e.children.front();
                    const std::wstring tk = lk + (isSpectrum ? L".a" + std::to_wstring(++a)
                                                  : isBar    ? L".r" + std::to_wstring(++r)
                                                             : L".t" + std::to_wstring(++t));
                    const std::wstring *own = x.Find(L"label"), *value = x.Find(L"value");
                    const wchar_t *data = value ? DataLabel(*value) : nullptr;
                    if (isSpectrum) ++spectra;
                    else if (isBar) ++bars;
                    else ++texts;
                    std::wstring called = own ? *own : data ? std::wstring(data) : L"";
                    if (called.empty())
                        called = isSpectrum ? Format(L"频谱 %zu", spectra)
                                 : isBar    ? Format(L"进度条 %zu", bars)
                                            : Format(L"文字 %zu", texts);
                    Part text{tk, called, {}};
                    offer(text, isSpectrum ? L"spectrum" : isBar ? L"bar" : L"text", tk);
                    // The line's distance from the one above goes with its first text or bar.
                    if (lines > 1 && first) offer(text, L"line", lk);
                    // Whether a text hangs beside the others (an AM/PM marker beside the numbers)
                    // is a choice only where the component offers it, by writing `hang` on it.
                    if (!x.Find(L"hang")) {
                        auto &adj = text.adjustments;
                        adj.erase(std::remove_if(adj.begin(), adj.end(),
                                                 [](const Adjustment &a) {
                                                     return a.key.size() > 5 &&
                                                            a.key.compare(a.key.size() - 5, 5, L".hang") == 0;
                                                 }),
                                  adj.end());
                    }
                    if (first) {  // and space before one only means something after another
                        auto &adj = text.adjustments;
                        adj.erase(std::remove_if(adj.begin(), adj.end(),
                                                 [](const Adjustment &a) {
                                                     return a.key.size() > 6 &&
                                                            a.key.compare(a.key.size() - 6, 6, L".space") == 0;
                                                 }),
                                  adj.end());
                    }
                    parts.push_back(std::move(text));
                }
            } else if (e.name == L"shadow") {
                const std::wstring sk = pk + L".s" + std::to_wstring(++shadows);
                const std::wstring *own = e.Find(L"label");
                Part shadow{sk, own ? *own : Format(L"阴影 %zu", shadows), {}};
                offer(shadow, L"shadow", sk);
                parts.push_back(std::move(shadow));
            } else if (e.name == L"backdrop") {
                const std::wstring *own = e.Find(L"label");
                Part backdrop{pk + L".b", own ? *own : L"背景遮罩", {}};
                offer(backdrop, L"backdrop", pk + L".b");
                parts.push_back(std::move(backdrop));
            }
        }
    }
    return parts;
}

std::wstring Effective(const Skin &skin, const Values &values, const Adjustment &adjustment) {
    const size_t dot = adjustment.key.rfind(L'.');
    const xml::Element *e = dot == std::wstring::npos ? nullptr : ElementAt(skin, adjustment.key.substr(0, dot));
    if (!e) return L"";
    const std::wstring attribute = adjustment.key.substr(dot + 1);
    const Offer *offer = OfferFor(e->name, attribute);
    Applied a(skin, values, *e, adjustment.key.substr(0, dot));
    std::wstring v = a.String(attribute.c_str(), offer ? offer->fallback : L"");
    if (attribute == L"weight") {
        const int w = WeightOf(v);
        v = std::to_wstring(w ? w : 400);
    }
    return v;
}

bool IsAdjustmentKey(const std::wstring &key) {
    std::wstring element, attribute;
    return KeyShape(key, &element, &attribute);
}

bool IsAdjustmentValue(const std::wstring &key, const std::wstring &value) {
    std::wstring element, attribute;
    if (!KeyShape(key, &element, &attribute)) return false;
    const AttributeSpec *spec = SpecFor(element, attribute);
    return spec && Check(*spec, value);
}

bool Accepts(const Skin &skin, const std::wstring &key, const std::wstring &value) {
    // Setting ids never contain a dot and adjustment keys always do.
    if (const Setting *setting = skin.Find(key)) return IsValue(*setting, value);
    std::wstring element, attribute;
    if (!KeyShape(key, &element, &attribute)) return false;
    const xml::Element *e = ElementAt(skin, key.substr(0, key.rfind(L'.')));
    return e && e->name == element && IsAdjustmentValue(key, value);
}

std::vector<std::wstring> AdjustmentsOf(const Skin &skin, const std::wstring &setting) {
    std::vector<std::wstring> keys;
    for (const Part &part : Parts(skin))
        for (const Adjustment &a : part.adjustments) {
            const size_t dot = a.key.rfind(L'.');
            const xml::Element *e = ElementAt(skin, a.key.substr(0, dot));
            const std::wstring *v = e ? e->Find(a.key.substr(dot + 1).c_str()) : nullptr;
            if (v && *v == L"$" + setting) keys.push_back(a.key);
        }
    return keys;
}

bool Parse(std::string_view utf8, Skin *skin, std::wstring *error) {
    Skin parsed;
    if (!xml::Parse(utf8, &parsed.document, error)) return false;
    Checker checker(&parsed);
    if (!checker.Run()) {
        if (error) *error = checker.error();
        return false;
    }
    *skin = std::move(parsed);
    return true;
}

std::string Normalize(const Skin &skin) {
    xml::Element root = skin.document;
    root.name = L"component";
    return xml::Write(root);
}

Resolved Resolve(const Skin &skin, const Values &values) {
    Resolved out;
    size_t panels = 0;
    for (const xml::Element &p : skin.document.children) {
        if (p.name != L"panel") continue;
        Panel panel;
        const std::wstring pk = L"p" + std::to_wstring(++panels);
        Applied a(skin, values, p, pk);
        const int anchor = IndexOf(kAnchors, a.String(L"anchor", L"top"));
        panel.anchor = anchor < 0 ? 1 : anchor;
        panel.size = a.Number(L"size", panel.size);
        panel.margin = a.Number(L"margin", panel.margin);
        panel.offsetX = a.Number(L"offset-x", 0.0f);
        panel.offsetY = a.Number(L"offset-y", 0.0f);
        const std::wstring align = a.String(L"align", L"auto");
        panel.align = align == L"left" ? Align::Left : align == L"center" ? Align::Center : align == L"right" ? Align::Right
                                                                                                           : Align::Auto;
        panel.when = ConditionOf(p);
        data::ParseDate(a.String(L"until"), &panel.until);
        const std::wstring enter = a.String(L"enter", L"fade");
        panel.enter = enter == L"rise" ? Enter::Rise : enter == L"zoom" ? Enter::Zoom : enter == L"none" ? Enter::None : Enter::Fade;
        panel.enterDelay = a.Number(L"enter-delay", panel.enterDelay);
        panel.enterDuration = a.Number(L"enter-duration", panel.enterDuration);
        size_t lines = 0, shadows = 0;
        for (const xml::Element &e : p.children) {
            const std::wstring key = e.name == L"line"     ? pk + L".l" + std::to_wstring(++lines)
                                     : e.name == L"shadow" ? pk + L".s" + std::to_wstring(++shadows)
                                                           : pk + L".b";
            Applied c(skin, values, e, key);
            if (e.name == L"line") {
                Line line;
                line.gap = c.Number(L"gap", line.gap);
                line.when = ConditionOf(e);
                size_t texts = 0, bars = 0, spectra = 0;
                for (const xml::Element &t : e.children) {
                    Text text;
                    text.bar = t.name == L"bar";
                    text.spectrum = t.name == L"spectrum";
                    text.name = text.spectrum ? L"a" + std::to_wstring(++spectra)
                                : text.bar    ? L"r" + std::to_wstring(++bars)
                                              : L"t" + std::to_wstring(++texts);
                    Applied ta(skin, values, t, key + L"." + text.name);
                    text.when = ConditionOf(t);
                    if (text.spectrum) {
                        ReadSpectrum(ta, &text);
                        line.texts.push_back(std::move(text));
                        continue;
                    }
                    text.value = ta.String(L"value");
                    if (text.bar) {
                        text.length = ta.Number(L"length", text.length);
                        text.thickness = ta.Number(L"thickness", text.thickness);
                        text.radius = ta.Number(L"radius", text.radius);
                        text.color = ta.Color(L"color", text.color);
                        text.opacity = ta.Number(L"opacity", 1.0f);
                        text.track = ta.Color(L"track", text.track);
                        text.trackOpacity = ta.Number(L"track-opacity", text.trackOpacity);
                        text.space = ta.Number(L"space", 0.0f);
                        line.texts.push_back(std::move(text));
                        continue;
                    }
                    text.font = ta.String(L"font");
                    text.size = ta.Number(L"size", 1.0f);
                    const int weight = WeightOf(ta.String(L"weight", L"regular"));
                    text.weight = weight ? weight : 400;
                    text.italic = ta.String(L"style") == L"italic";
                    text.color = ta.Color(L"color", text.color);
                    text.opacity = ta.Number(L"opacity", 1.0f);
                    text.tracking = ta.Number(L"tracking", 0.0f);
                    text.space = ta.Number(L"space", 0.0f);
                    const std::wstring textCase = ta.String(L"case");
                    text.textCase = textCase == L"upper" ? Case::Upper : textCase == L"lower" ? Case::Lower : Case::None;
                    text.hang = ta.String(L"hang") == L"true";
                    line.texts.push_back(std::move(text));
                }
                panel.lines.push_back(std::move(line));
            } else if (e.name == L"shadow") {
                Shadow s;
                s.blur = c.Number(L"blur", s.blur);
                s.opacity = c.Number(L"opacity", s.opacity);
                s.x = c.Number(L"x", 0.0f);
                s.y = c.Number(L"y", 0.0f);
                s.color = c.Color(L"color", s.color);
                panel.shadows.push_back(s);
            } else if (e.name == L"backdrop") {
                panel.backdrop = c.Number(L"opacity", 0.0f);
                panel.backdropColor = c.Color(L"color", panel.backdropColor);
            }
        }
        out.panels.push_back(std::move(panel));
    }
    return out;
}

bool Listens(const Resolved &skin) {
    for (const Panel &p : skin.panels)
        for (const Line &l : p.lines)
            for (const Text &t : l.texts)
                if (t.spectrum) return true;
    return false;
}

std::string DefaultText() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<!-- The clock that comes with AnimeLogon. Export it from the settings app to start a component of your own. -->
<component format="1" name="时钟" author="AnimeLogon">
  <settings>
    <choice id="position" label="位置" detail="边距随分辨率自动调整。" default="top">
      <option value="top-left" label="左上"/>
      <option value="top" label="上方居中"/>
      <option value="top-right" label="右上"/>
      <option value="left" label="左侧居中"/>
      <option value="center" label="居中"/>
      <option value="right" label="右侧居中"/>
      <option value="bottom-left" label="左下"/>
      <option value="bottom" label="下方居中"/>
      <option value="bottom-right" label="右下"/>
    </choice>
    <number id="size" label="大小" detail="时间文字的字号，以屏幕高度的百分比计。" default="10" min="5" max="20" step="0.5"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" default="#FFFFFF"/>
    <toggle id="shade" label="背景遮罩" detail="在时钟下方加深背景，提高在浅色壁纸上的可读性。" default="on" on="0.22" off="0"/>
  </settings>
  <panel anchor="$position" size="$size" margin="7">
    <line>
      <text value="{time}" font="$font" weight="semibold" color="$color" tracking="-0.01"/>
      <text value="{ampm}" font="$font" weight="semibold" color="$color" size="0.36" space="0.1" hang="true"/>
    </line>
    <line gap="0.2">
      <text value="{date}" font="$font" weight="semibold" color="$color" size="0.2"/>
    </line>
    <shadow label="阴影" blur="0.08" opacity="0" y="0.02"/>
    <backdrop opacity="$shade"/>
  </panel>
</component>
)";
}

const Skin &Default() {
    static const Skin skin = [] {
        Skin s;
        Parse(DefaultText(), &s, nullptr);
        return s;
    }();
    return skin;
}

}  // namespace animelogon::skin
