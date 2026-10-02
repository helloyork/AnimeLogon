#include "animelogon/skin.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "animelogon/clock.h"
#include "animelogon/text.h"

namespace animelogon::skin {
namespace {

constexpr size_t kMaxSettings = 32, kMaxOptions = 32, kMaxPanels = 8, kMaxLines = 8, kMaxTexts = 8;
constexpr size_t kMaxShadows = 4, kMaxLabel = 40, kMaxDetail = 80, kMaxTemplate = 256;

const wchar_t *const kAnchors[] = {L"top-left", L"top", L"top-right", L"left", L"center",
                                   L"right", L"bottom-left", L"bottom", L"bottom-right"};
const wchar_t *const kDataNames[] = {L"time", L"ampm", L"date", L"date.long", L"weekday", L"month", L"day", L"year"};

enum class Type { Label, Anchor, Number, Align, Template, Font, Weight, Style, Color, Case, Bool };

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
                                {L"align", Type::Align}};
const AttributeSpec kLine[] = {{L"label", Type::Label, 0, 0, false}, {L"gap", Type::Number, -2, 4}};
const AttributeSpec kText[] = {{L"label", Type::Label, 0, 0, false}, {L"value", Type::Template, 0, 0, false},
                               {L"font", Type::Font},                {L"size", Type::Number, 0.05, 4},
                               {L"weight", Type::Weight},            {L"style", Type::Style},
                               {L"color", Type::Color},              {L"opacity", Type::Number, 0, 1},
                               {L"tracking", Type::Number, -0.5, 1}, {L"space", Type::Number, 0, 4},
                               {L"case", Type::Case},                {L"hang", Type::Bool, 0, 0, false}};
const AttributeSpec kShadow[] = {{L"label", Type::Label, 0, 0, false},
                                 {L"blur", Type::Number, 0, 1},
                                 {L"opacity", Type::Number, 0, 1},
                                 {L"x", Type::Number, -1, 1},
                                 {L"y", Type::Number, -1, 1},
                                 {L"color", Type::Color}};
const AttributeSpec kBackdrop[] = {
    {L"label", Type::Label, 0, 0, false}, {L"opacity", Type::Number, 0, 1}, {L"color", Type::Color}};

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
    case SettingKind::Number: return spec.type == Type::Number && setting.min >= spec.min && setting.max <= spec.max;
    case SettingKind::Color: return spec.type == Type::Color;
    case SettingKind::Font: return spec.type == Type::Font;
    }
    return false;
}

class Checker {
public:
    explicit Checker(Skin *skin) : skin_(skin) {}

    bool Run() {
        const xml::Element &root = skin_->document;
        // <skin> is the root's name from before components; transitional, until the overlay and
        // the settings app use themes. Normalize always writes <component>.
        if (root.name != L"component" && root.name != L"skin") return Fail(root, L"the root element must be <component>");
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
                if (e.children.empty() || e.children.size() > kMaxTexts) return Fail(e, L"a line needs 1 to 8 texts");
                for (const xml::Element &t : e.children) {
                    if (t.name != L"text") return Fail(t, L"a line holds only <text>");
                    if (!t.children.empty()) return Fail(t, L"<text> holds nothing");
                    if (!t.Find(L"value")) return Fail(t, L"<text> needs a value");
                    if (!Attributes(t, kText)) return false;
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
    {L"panel", L"anchor", L"位置", L"相对屏幕的哪个位置。", Control::Choice, 0, 0, 0, L"top"},
    {L"panel", L"offset-x", L"水平偏移", L"屏幕宽度的百分比，向右为正。", Control::Slider, -50, 50, 0.1, L"0"},
    {L"panel", L"offset-y", L"垂直偏移", L"屏幕高度的百分比，向下为正。", Control::Slider, -50, 50, 0.1, L"0"},
    {L"panel", L"margin", L"边距", L"离屏幕边缘的距离，短边的百分比。", Control::Slider, 0, 40, 0.5, L"7"},
    {L"panel", L"size", L"大小", L"屏幕高度的百分比，其余文字按它的比例。", Control::Slider, 1, 50, 0.5, L"10"},
    {L"panel", L"align", L"对齐", L"多行文字之间如何对齐。", Control::Choice, 0, 0, 0, L"auto"},
    {L"text", L"font", L"字体", L"仅列出为所有用户安装的字体。", Control::Font, 0, 0, 0, L""},
    {L"text", L"weight", L"字重", L"字体没有的字重会用最接近的代替。", Control::Slider, 100, 900, 50, L"400"},
    {L"text", L"size", L"相对大小", L"相对于整体大小。", Control::Slider, 0.05, 2, 0.01, L"1"},
    {L"text", L"tracking", L"字距", L"相对于这段文字的字号。", Control::Slider, -0.2, 0.5, 0.005, L"0"},
    {L"text", L"space", L"前方留白", L"与前一段文字的距离，相对于整体大小。", Control::Slider, 0, 2, 0.01, L"0"},
    {L"line", L"gap", L"与上一行的距离", L"从上一行的基线算起，相对于整体大小。", Control::Slider, -1, 2, 0.01, L"0.2"},
    {L"text", L"color", L"颜色", L"也可以直接输入 #RRGGBB 或 #RRGGBBAA。", Control::Color, 0, 0, 0, L"#FFFFFF"},
    {L"text", L"opacity", L"不透明度", L"", Control::Slider, 0, 1, 0.01, L"1"},
    {L"text", L"style", L"字形", L"", Control::Choice, 0, 0, 0, L"normal"},
    {L"text", L"case", L"大小写", L"只影响拉丁字母。", Control::Choice, 0, 0, 0, L"none"},
    {L"shadow", L"opacity", L"浓度", L"0 为不画阴影。", Control::Slider, 0, 1, 0.01, L"0.3"},
    {L"shadow", L"blur", L"模糊", L"相对于整体大小。", Control::Slider, 0, 0.5, 0.005, L"0.1"},
    {L"shadow", L"x", L"水平偏移", L"相对于整体大小。", Control::Slider, -0.3, 0.3, 0.005, L"0"},
    {L"shadow", L"y", L"垂直偏移", L"相对于整体大小。", Control::Slider, -0.3, 0.3, 0.005, L"0"},
    {L"shadow", L"color", L"颜色", L"", Control::Color, 0, 0, 0, L"#000000"},
    {L"backdrop", L"opacity", L"浓度", L"在文字背后压暗画面；0 为不画。", Control::Slider, 0, 1, 0.01, L"0"},
    {L"backdrop", L"color", L"颜色", L"", Control::Color, 0, 0, 0, L"#000000"},
};

std::vector<Option> OptionsOf(const std::wstring &attribute) {
    if (attribute == L"anchor")
        return {{L"top-left", L"左上"},    {L"top", L"上方居中"},       {L"top-right", L"右上"},
                {L"left", L"左侧居中"},    {L"center", L"居中"},        {L"right", L"右侧居中"},
                {L"bottom-left", L"左下"}, {L"bottom", L"下方居中"},    {L"bottom-right", L"右下"}};
    if (attribute == L"align")
        return {{L"auto", L"自动"}, {L"left", L"左"}, {L"center", L"中"}, {L"right", L"右"}};
    if (attribute == L"style") return {{L"normal", L"常规"}, {L"italic", L"斜体"}};
    if (attribute == L"case") return {{L"none", L"原样"}, {L"upper", L"大写"}, {L"lower", L"小写"}};
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
                            : kind == L's' ? L"shadow" : kind == L'b' ? L"backdrop" : nullptr;
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
    else return false;
    const AttributeSpec *spec = SpecFor(*element, *attribute);
    return spec && spec->adjustable;
}

const wchar_t *DataLabel(const std::wstring &value) {
    static const struct {
        const wchar_t *value, *label;
    } known[] = {{L"{time}", L"时间"},      {L"{ampm}", L"上午/下午"}, {L"{date}", L"日期"},
                 {L"{date.long}", L"日期"}, {L"{weekday}", L"星期"},   {L"{month}", L"月份"},
                 {L"{day}", L"日"},         {L"{year}", L"年份"}};
    for (const auto &k : known)
        if (value == k.value) return k.label;
    return nullptr;
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

bool IsSkinId(const std::wstring &id) {
    if (id == L"default") return true;
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

bool IsDataName(const std::wstring &name) {
    for (const wchar_t *d : kDataNames)
        if (name == d) return true;
    return false;
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
        size_t lines = 0, shadows = 0, texts = 0;
        for (const xml::Element &e : p.children) {
            if (e.name == L"line") {
                const std::wstring lk = pk + L".l" + std::to_wstring(++lines);
                size_t t = 0;
                for (const xml::Element &x : e.children) {
                    const std::wstring tk = lk + L".t" + std::to_wstring(++t);
                    const std::wstring *own = x.Find(L"label"), *value = x.Find(L"value");
                    const wchar_t *data = value ? DataLabel(*value) : nullptr;
                    ++texts;
                    Part text{tk, own ? *own : data ? std::wstring(data) : Format(L"文字 %zu", texts), {}};
                    offer(text, L"text", tk);
                    // The line's distance from the one above goes with its first text.
                    if (lines > 1 && t == 1) offer(text, L"line", lk);
                    if (t == 1) {  // and space before a text only means something after another
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
                Part backdrop{pk + L".b", own ? *own : L"暗晕", {}};
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
        size_t lines = 0, shadows = 0;
        for (const xml::Element &e : p.children) {
            const std::wstring key = e.name == L"line"     ? pk + L".l" + std::to_wstring(++lines)
                                     : e.name == L"shadow" ? pk + L".s" + std::to_wstring(++shadows)
                                                           : pk + L".b";
            Applied c(skin, values, e, key);
            if (e.name == L"line") {
                Line line;
                line.gap = c.Number(L"gap", line.gap);
                size_t texts = 0;
                for (const xml::Element &t : e.children) {
                    Applied ta(skin, values, t, key + L".t" + std::to_wstring(++texts));
                    Text text;
                    text.value = ta.String(L"value");
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
    <number id="size" label="大小" detail="时间的字号，占屏幕高度的百分比。" default="10" min="5" max="20" step="0.5"/>
    <font id="font" label="字体" detail="仅列出为所有用户安装的字体。" default=""/>
    <color id="color" label="颜色" detail="也可以直接输入 #RRGGBB。" default="#FFFFFF"/>
    <toggle id="shade" label="背景暗晕" detail="在时钟背后稍微压暗画面，亮色壁纸上也能看清。" default="on" on="0.22" off="0"/>
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
