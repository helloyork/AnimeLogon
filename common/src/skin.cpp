#include "animelogon/skin.h"

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

enum class Type { Anchor, Number, Align, Locale, Hours, Template, Font, Weight, Style, Color, Case, Bool };

struct AttributeSpec {
    const wchar_t *name;
    Type type;
    double min = 0, max = 0;
};

const AttributeSpec kPanel[] = {{L"anchor", Type::Anchor},          {L"size", Type::Number, 1, 50},
                                {L"margin", Type::Number, 0, 40},    {L"offset-x", Type::Number, -50, 50},
                                {L"offset-y", Type::Number, -50, 50}, {L"align", Type::Align},
                                {L"locale", Type::Locale},           {L"hours", Type::Hours}};
const AttributeSpec kLine[] = {{L"gap", Type::Number, -2, 4}};
const AttributeSpec kText[] = {{L"value", Type::Template},          {L"font", Type::Font},
                               {L"size", Type::Number, 0.05, 4},     {L"weight", Type::Weight},
                               {L"style", Type::Style},             {L"color", Type::Color},
                               {L"opacity", Type::Number, 0, 1},     {L"tracking", Type::Number, -0.5, 1},
                               {L"space", Type::Number, 0, 4},       {L"case", Type::Case},
                               {L"hang", Type::Bool}};
const AttributeSpec kShadow[] = {{L"blur", Type::Number, 0, 1},  {L"opacity", Type::Number, 0, 1},
                                 {L"x", Type::Number, -1, 1},     {L"y", Type::Number, -1, 1},
                                 {L"color", Type::Color}};
const AttributeSpec kBackdrop[] = {{L"opacity", Type::Number, 0, 1}, {L"color", Type::Color}};

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

bool IsLocaleName(const std::wstring &text) {
    if (text.size() > 32) return false;
    for (wchar_t c : text)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-'))
            return false;
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
    case Type::Anchor: return IndexOf(kAnchors, value) >= 0;
    case Type::Number: return ParseNumber(value, &v) && v >= spec.min && v <= spec.max;
    case Type::Align: return value == L"auto" || value == L"left" || value == L"center" || value == L"right";
    case Type::Locale: return IsLocaleName(value);
    case Type::Hours: return value == L"auto" || value == L"12" || value == L"24";
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
        if (root.name != L"skin") return Fail(root, L"the root element must be <skin>");
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
        if (!root.Find(L"format") || skin_->name.empty()) return Fail(root, L"<skin> needs format and name");
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
            if (!value.empty() && value[0] == L'$') {
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
    Applied(const Skin &skin, const Values &values, const xml::Element &e) : skin_(skin), values_(values), e_(e) {}

    const std::wstring *Raw(const wchar_t *name) {
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
    std::wstring resolved_;
};

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

std::string Normalize(const Skin &skin) { return xml::Write(skin.document); }

Resolved Resolve(const Skin &skin, const Values &values) {
    Resolved out;
    for (const xml::Element &p : skin.document.children) {
        if (p.name != L"panel") continue;
        Panel panel;
        Applied a(skin, values, p);
        const int anchor = IndexOf(kAnchors, a.String(L"anchor", L"top"));
        panel.anchor = anchor < 0 ? 1 : anchor;
        panel.size = a.Number(L"size", panel.size);
        panel.margin = a.Number(L"margin", panel.margin);
        panel.offsetX = a.Number(L"offset-x", 0.0f);
        panel.offsetY = a.Number(L"offset-y", 0.0f);
        const std::wstring align = a.String(L"align", L"auto");
        panel.align = align == L"left" ? Align::Left : align == L"center" ? Align::Center : align == L"right" ? Align::Right
                                                                                                           : Align::Auto;
        panel.locale = a.String(L"locale");
        const std::wstring hours = a.String(L"hours", L"auto");
        panel.hours = hours == L"12" ? Hours::H12 : hours == L"24" ? Hours::H24 : Hours::Auto;
        for (const xml::Element &e : p.children) {
            Applied c(skin, values, e);
            if (e.name == L"line") {
                Line line;
                line.gap = c.Number(L"gap", line.gap);
                for (const xml::Element &t : e.children) {
                    Applied ta(skin, values, t);
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
<!-- The clock that comes with AnimeLogon. Export it from the settings app to start a skin of your own. -->
<skin format="1" name="时钟" author="AnimeLogon">
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
    <choice id="weight" label="字重" detail="字体没有的字重会用最接近的代替。" default="semibold">
      <option value="light" label="细"/>
      <option value="regular" label="常规"/>
      <option value="semibold" label="半粗"/>
      <option value="bold" label="粗"/>
    </choice>
    <color id="color" label="颜色" detail="也可以直接输入 #RRGGBB。" default="#FFFFFF"/>
    <toggle id="shade" label="背景暗晕" detail="在时钟背后稍微压暗画面，亮色壁纸上也能看清。" default="on" on="0.22" off="0"/>
    <choice id="date" label="日期" detail="完整日期与任务栏的长日期一致。" default="{date}">
      <option value="{date}" label="星期与月日"/>
      <option value="{date.long}" label="完整日期"/>
      <option value="" label="不显示"/>
    </choice>
    <choice id="hours" label="时间制式" detail="跟随区域格式时与任务栏时钟一致。" default="auto">
      <option value="auto" label="跟随区域格式"/>
      <option value="12" label="12 小时制"/>
      <option value="24" label="24 小时制"/>
    </choice>
    <toggle id="ampm" label="显示上午/下午" detail="12 小时制时，以小字显示在时间旁边。" default="on" on="{ampm}" off=""/>
    <choice id="language" label="语言" detail="时间和日期的文字语言。" default="">
      <option value="" label="跟随区域格式"/>
      <option value="zh-CN" label="简体中文"/>
      <option value="zh-TW" label="繁體中文"/>
      <option value="ja-JP" label="日本語"/>
      <option value="ko-KR" label="한국어"/>
      <option value="en-US" label="English"/>
    </choice>
  </settings>
  <panel anchor="$position" size="$size" margin="7" locale="$language" hours="$hours">
    <line>
      <text value="{time}" font="$font" weight="$weight" color="$color" tracking="-0.01"/>
      <text value="$ampm" font="$font" weight="$weight" color="$color" size="0.36" space="0.1" hang="true"/>
    </line>
    <line gap="0.2">
      <text value="$date" font="$font" weight="$weight" color="$color" size="0.2"/>
    </line>
    <shadow blur="0.12" opacity="0.28" y="0.024"/>
    <shadow blur="0.012" opacity="0.35"/>
    <backdrop opacity="$shade"/>
  </panel>
</skin>
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
