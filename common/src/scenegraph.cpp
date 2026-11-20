#include "animelogon/scenegraph.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <set>
#include "animelogon/text.h"

namespace animelogon::scenegraph {
namespace {
constexpr double kPi = 3.14159265358979323846;

const wchar_t *const kAnchors[] = {L"top-left", L"top", L"top-right", L"left", L"center", L"right",
                                   L"bottom-left", L"bottom", L"bottom-right", nullptr};
const wchar_t *const kSpaces[] = {L"stage", L"screen", nullptr};
const wchar_t *const kFits[] = {L"cover", L"contain", L"stretch", nullptr};
const wchar_t *const kBlends[] = {L"normal", L"add", L"screen", L"multiply", nullptr};
const wchar_t *const kRepeats[] = {L"once", L"loop", L"bounce", nullptr};
const wchar_t *const kEases[] = {L"linear", L"smooth", L"in", L"out", L"step", L"bezier", nullptr};
const wchar_t *const kGradients[] = {L"vertical", L"horizontal", nullptr};
const wchar_t *const kPresets[] = {L"rain", L"snow", L"petals", L"fireflies", L"stars", L"dust", nullptr};
const wchar_t *const kEmits[] = {L"top", L"bottom", L"left", L"right", L"point", L"line", L"area", nullptr};
const wchar_t *const kRoots[] = {L"top", L"bottom", L"left", L"right", nullptr};
const wchar_t *const kDirections[] = {L"horizontal", L"vertical", nullptr};

enum class Type { Number, Integer, Bool, Enum, Color, Colors, Pair, Range, Asset, Vector };
// One attribute a kind of node or effect takes. `animated` ones can be the target of a motion.
struct Prop {
    const wchar_t *name;
    Type type;
    double lo, hi;
    const wchar_t *const *words;
    bool animated;
    const wchar_t *fallback;
};
constexpr double kBig = 1e5;

const Prop kCommon[] = {
    {L"x", Type::Number, -kBig, kBig, nullptr, true, L"0"},
    {L"y", Type::Number, -kBig, kBig, nullptr, true, L"0"},
    {L"width", Type::Number, 0, kBig, nullptr, true, L"0"},
    {L"height", Type::Number, 0, kBig, nullptr, true, L"0"},
    {L"anchor", Type::Enum, 0, 0, kAnchors, false, L"top-left"},
    {L"space", Type::Enum, 0, 0, kSpaces, false, L"stage"},
    {L"full", Type::Bool, 0, 0, nullptr, false, L"false"},
    {L"fit", Type::Enum, 0, 0, kFits, false, L"cover"},
    {L"pivot", Type::Pair, 0, 1, nullptr, false, L"0.5 0.5"},
    {L"rotation", Type::Number, -36000, 36000, nullptr, true, L"0"},
    {L"scale", Type::Number, 0, 100, nullptr, true, L"1"},
    {L"scale-x", Type::Number, 0, 100, nullptr, true, L"1"},
    {L"scale-y", Type::Number, 0, 100, nullptr, true, L"1"},
    {L"opacity", Type::Number, 0, 1, nullptr, true, L"1"},
    {L"blend", Type::Enum, 0, 0, kBlends, false, L"normal"},
    {L"depth", Type::Number, 0, 1, nullptr, false, L"1"},
    {L"mask", Type::Asset, 0, 0, nullptr, false, L""},
};
const Prop kLayer[] = {
    {L"source", Type::Asset, 0, 0, nullptr, false, L""},
    {L"fps", Type::Number, 1, 60, nullptr, false, L"24"},
    {L"repeat", Type::Enum, 0, 0, kRepeats, false, L"loop"},
    {L"gap", Type::Range, 0, 3600, nullptr, false, L""},
};
const Prop kFill[] = {
    {L"color", Type::Color, 0, 0, nullptr, true, L"#000000"},
    {L"colors", Type::Colors, 0, 0, nullptr, false, L""},
    {L"gradient", Type::Enum, 0, 0, kGradients, false, L"vertical"},
};
const Prop kParticles[] = {
    {L"source", Type::Asset, 0, 0, nullptr, false, L""},
    {L"preset", Type::Enum, 0, 0, kPresets, false, L""},
    {L"rate", Type::Number, 0, 5000, nullptr, true, L"20"},
    {L"lifetime", Type::Number, 0.1, 60, nullptr, false, L"4"},
    {L"emit", Type::Enum, 0, 0, kEmits, false, L"top"},
    {L"speed", Type::Number, 0, 10000, nullptr, true, L"100"},
    {L"angle", Type::Number, -360, 360, nullptr, false, L"90"},
    {L"spread", Type::Number, 0, 360, nullptr, false, L"10"},
    {L"gravity", Type::Number, -10000, 10000, nullptr, false, L"0"},
    {L"wind", Type::Number, -10000, 10000, nullptr, true, L"0"},
    {L"drag", Type::Number, 0, 10, nullptr, false, L"0"},
    {L"spin", Type::Number, -3600, 3600, nullptr, false, L"0"},
    {L"size", Type::Number, 0, 2000, nullptr, true, L"16"},
    {L"size-end", Type::Number, 0, 2000, nullptr, false, L""},
    {L"opacity-end", Type::Number, 0, 1, nullptr, false, L""},
    {L"color", Type::Color, 0, 0, nullptr, true, L"#FFFFFF"},
    {L"color-end", Type::Color, 0, 0, nullptr, false, L""},
    {L"fade", Type::Number, 0, 0.5, nullptr, false, L"0.1"},
    {L"seed", Type::Integer, 0, 1000000, nullptr, false, L"1"},
    {L"max", Type::Integer, 1, (double)kMaxParticles, nullptr, false, L"2000"},
    {L"fps", Type::Number, 1, 60, nullptr, false, L"24"},
    {L"sway", Type::Number, 0, 1000, nullptr, true, L"0"},     // side-to-side drift, in points
    {L"twinkle", Type::Number, 0, 1, nullptr, true, L"0"},     // how far each particle's brightness pulses
    {L"stretch", Type::Number, 0, 1, nullptr, false, L"0"},    // drawn as a streak this many seconds of motion long
};

// What each particle preset changes from the plain defaults; anything written on the element wins.
struct PresetValue {
    const wchar_t *preset, *name, *value;
};
const PresetValue kPresetValues[] = {
    {L"rain", L"rate", L"300"}, {L"rain", L"lifetime", L"1.2"}, {L"rain", L"speed", L"1400"}, {L"rain", L"angle", L"100"},
    {L"rain", L"spread", L"4"}, {L"rain", L"size", L"2"}, {L"rain", L"stretch", L"0.03"}, {L"rain", L"color", L"#B8CCFF99"},
    {L"rain", L"fade", L"0.05"}, {L"rain", L"max", L"1500"},
    {L"snow", L"rate", L"40"}, {L"snow", L"lifetime", L"14"}, {L"snow", L"speed", L"50"}, {L"snow", L"spread", L"30"},
    {L"snow", L"sway", L"25"}, {L"snow", L"size", L"7"}, {L"snow", L"color", L"#FFFFFFE6"}, {L"snow", L"max", L"800"},
    {L"petals", L"rate", L"12"}, {L"petals", L"lifetime", L"14"}, {L"petals", L"speed", L"70"}, {L"petals", L"angle", L"100"},
    {L"petals", L"spread", L"40"}, {L"petals", L"sway", L"45"}, {L"petals", L"spin", L"120"}, {L"petals", L"size", L"16"},
    {L"petals", L"color", L"#FFC4D6"}, {L"petals", L"max", L"600"},
    {L"fireflies", L"rate", L"4"}, {L"fireflies", L"lifetime", L"7"}, {L"fireflies", L"emit", L"area"}, {L"fireflies", L"speed", L"15"},
    {L"fireflies", L"spread", L"360"}, {L"fireflies", L"drag", L"0.4"}, {L"fireflies", L"sway", L"30"}, {L"fireflies", L"twinkle", L"0.8"},
    {L"fireflies", L"size", L"9"}, {L"fireflies", L"color", L"#FFE680"}, {L"fireflies", L"fade", L"0.2"}, {L"fireflies", L"max", L"120"},
    {L"fireflies", L"blend", L"add"},
    {L"stars", L"rate", L"60"}, {L"stars", L"lifetime", L"10"}, {L"stars", L"emit", L"area"}, {L"stars", L"speed", L"0"},
    {L"stars", L"spread", L"0"}, {L"stars", L"twinkle", L"0.9"}, {L"stars", L"size", L"4"}, {L"stars", L"fade", L"0.3"},
    {L"stars", L"max", L"800"}, {L"stars", L"blend", L"add"},
    {L"dust", L"rate", L"15"}, {L"dust", L"lifetime", L"10"}, {L"dust", L"emit", L"area"}, {L"dust", L"speed", L"6"},
    {L"dust", L"spread", L"360"}, {L"dust", L"sway", L"12"}, {L"dust", L"twinkle", L"0.3"}, {L"dust", L"size", L"3"},
    {L"dust", L"color", L"#FFF4E080"}, {L"dust", L"fade", L"0.25"}, {L"dust", L"max", L"300"}, {L"dust", L"blend", L"add"},
};
const wchar_t *PresetDefault(const std::wstring &preset, const wchar_t *name) {
    for (const auto &v : kPresetValues)
        if (preset == v.preset && !wcscmp(name, v.name)) return v.value;
    return nullptr;
}

struct EffectSpec {
    const wchar_t *type;
    std::vector<Prop> props;
};
// Our own effects. Time-driven ones animate by themselves; every number and color can also be
// the target of a motion.
const std::vector<EffectSpec> &Effects() {
    static const std::vector<EffectSpec> specs = {
        {L"bend", {{L"strength", Type::Number, 0, 1, nullptr, true, L"0.5"}, {L"period", Type::Number, 0.1, 60, nullptr, true, L"3"},
                   {L"root", Type::Enum, 0, 0, kRoots, false, L"top"}, {L"waves", Type::Number, 0, 4, nullptr, true, L"0.25"}}},
        {L"breathe", {{L"amount", Type::Number, 0, 0.2, nullptr, true, L"0.01"}, {L"period", Type::Number, 0.1, 60, nullptr, true, L"4"},
                      {L"pivot", Type::Pair, 0, 1, nullptr, false, L"0.5 1"}}},
        {L"wave", {{L"amplitude", Type::Number, 0, 0.2, nullptr, true, L"0.01"}, {L"length", Type::Number, 0.01, 2, nullptr, true, L"0.2"},
                   {L"period", Type::Number, 0.1, 60, nullptr, true, L"2"}, {L"direction", Type::Enum, 0, 0, kDirections, false, L"horizontal"}}},
        {L"rings", {{L"center", Type::Pair, 0, 1, nullptr, false, L"0.5 0.5"}, {L"amplitude", Type::Number, 0, 0.1, nullptr, true, L"0.01"},
                    {L"length", Type::Number, 0.01, 1, nullptr, true, L"0.1"}, {L"speed", Type::Number, 0, 10, nullptr, true, L"1"}}},
        {L"flow", {{L"direction", Type::Number, -360, 360, nullptr, false, L"90"}, {L"speed", Type::Number, 0, 10, nullptr, true, L"0.2"},
                   {L"amount", Type::Number, 0, 0.2, nullptr, true, L"0.02"}}},
        {L"pan", {{L"speed-x", Type::Number, -10, 10, nullptr, true, L"0"}, {L"speed-y", Type::Number, -10, 10, nullptr, true, L"0"}}},
        {L"jitter", {{L"amount", Type::Number, 0, 0.1, nullptr, true, L"0.005"}, {L"rate", Type::Number, 0.1, 60, nullptr, true, L"12"}}},
        {L"glow", {{L"strength", Type::Number, 0, 4, nullptr, true, L"1"}, {L"radius", Type::Number, 0, 1, nullptr, true, L"0.02"},
                   {L"threshold", Type::Number, 0, 1, nullptr, true, L"0.6"}, {L"color", Type::Color, 0, 0, nullptr, true, L"#FFFFFF"}}},
        {L"rays", {{L"center", Type::Pair, 0, 1, nullptr, false, L"0.5 0"}, {L"strength", Type::Number, 0, 4, nullptr, true, L"1"},
                   {L"length", Type::Number, 0, 1, nullptr, true, L"0.5"}, {L"color", Type::Color, 0, 0, nullptr, true, L"#FFFFFF"}}},
        {L"sheen", {{L"angle", Type::Number, -360, 360, nullptr, false, L"30"}, {L"width", Type::Number, 0, 1, nullptr, true, L"0.15"},
                    {L"period", Type::Number, 0.1, 60, nullptr, true, L"4"}, {L"strength", Type::Number, 0, 4, nullptr, true, L"1"},
                    {L"color", Type::Color, 0, 0, nullptr, true, L"#FFFFFF"}}},
        {L"flicker", {{L"amount", Type::Number, 0, 1, nullptr, true, L"0.3"}, {L"rate", Type::Number, 0.1, 30, nullptr, true, L"3"}}},
        {L"grade", {{L"brightness", Type::Number, -1, 1, nullptr, true, L"0"}, {L"contrast", Type::Number, 0, 3, nullptr, true, L"1"},
                    {L"saturation", Type::Number, 0, 3, nullptr, true, L"1"}, {L"temperature", Type::Number, -1, 1, nullptr, true, L"0"}}},
        {L"tint", {{L"color", Type::Color, 0, 0, nullptr, true, L"#FFFFFF"}, {L"amount", Type::Number, 0, 1, nullptr, true, L"1"}}},
        {L"blur", {{L"radius", Type::Number, 0, 0.1, nullptr, true, L"0.01"}}},
        {L"haze", {{L"amount", Type::Number, 0, 0.05, nullptr, true, L"0.005"}, {L"scale", Type::Number, 0.01, 1, nullptr, true, L"0.1"},
                   {L"speed", Type::Number, 0, 10, nullptr, true, L"1"}}},
        {L"split", {{L"amount", Type::Number, 0, 0.02, nullptr, true, L"0.002"}, {L"angle", Type::Number, -360, 360, nullptr, true, L"0"}}},
        {L"grain", {{L"amount", Type::Number, 0, 1, nullptr, true, L"0.1"}, {L"size", Type::Number, 0.5, 4, nullptr, true, L"1"}}},
    };
    return specs;
}
// Effects whose look changes from frame to frame without any motion.
bool TimeDriven(const std::wstring &type) {
    for (const wchar_t *t : {L"bend", L"breathe", L"wave", L"rings", L"flow", L"pan", L"jitter", L"sheen", L"flicker", L"haze", L"grain"})
        if (type == t) return true;
    return false;
}
const EffectSpec *FindEffect(const std::wstring &type) {
    for (const auto &s : Effects()) if (type == s.type) return &s;
    return nullptr;
}

struct DataName { const wchar_t *name; const wchar_t *const *words; };
const wchar_t *const kPeriods[] = {L"dawn", L"day", L"dusk", L"night", nullptr};
const wchar_t *const kSeasons[] = {L"spring", L"summer", L"autumn", L"winter", nullptr};
const wchar_t *const kBatteryStates[] = {L"none", L"charging", L"discharging", L"plugged", nullptr};
const wchar_t *const kPowerSources[] = {L"none", L"battery", L"mains", nullptr};
const wchar_t *const kMediaStates[] = {L"none", L"playing", L"paused", nullptr};
// Data a scene can read. Numbers (words == nullptr) can drive a property; states can be compared.
const DataName kData[] = {
    {L"time", nullptr}, {L"clock.day", nullptr}, {L"wake", nullptr}, {L"battery.level", nullptr},
    {L"media.progress", nullptr}, {L"date.month", nullptr}, {L"audio.level", nullptr}, {L"audio.bass", nullptr},
    {L"audio.mid", nullptr}, {L"audio.treble", nullptr},
    {L"clock.period", kPeriods}, {L"date.season", kSeasons}, {L"battery.state", kBatteryStates},
    {L"power.source", kPowerSources}, {L"media.state", kMediaStates},
};
const DataName *FindData(std::wstring_view name) {
    for (const auto &d : kData) if (name == d.name) return &d;
    return nullptr;
}
bool InWords(const wchar_t *const *words, std::wstring_view w) {
    for (; words && *words; ++words) if (w == *words) return true;
    return false;
}
int IndexOf(const wchar_t *const *words, std::wstring_view w) {
    for (int i = 0; words && words[i]; ++i) if (w == words[i]) return i;
    return -1;
}

bool Number(std::wstring_view s, double *n) {
    if (s.empty() || s.size() > 32) return false;
    const std::wstring copy(s);
    wchar_t *end = nullptr;
    *n = wcstod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(*n) && std::abs(*n) <= 1e9 &&
           (iswdigit(copy[0]) || copy[0] == L'-' || copy[0] == L'.');
}
std::vector<std::wstring> Words(std::wstring_view s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == L' ') ++i;
        size_t j = i;
        while (j < s.size() && s[j] != L' ') ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
bool Color(std::wstring_view s) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != L'#') return false;
    for (size_t i = 1; i < s.size(); ++i) if (!iswxdigit(s[i])) return false;
    return true;
}
bool Stops(std::wstring_view s) {
    if (s.empty() || s.size() > 200) return false;
    double last = -1;
    size_t count = 0, start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L',', start);
        if (end == std::wstring_view::npos) end = s.size();
        const auto w = Words(s.substr(start, end - start));
        double at = 0;
        if (w.size() != 2 || !Color(w[0]) || w[1].size() < 2 || w[1].back() != L'%' ||
            !Number(std::wstring_view(w[1]).substr(0, w[1].size() - 1), &at) || at < 0 || at > 100 || at < last || ++count > 8)
            return false;
        last = at;
        if (end == s.size()) break;
        start = end + 1;
    }
    return count > 0;
}
bool Label(const std::wstring &s, size_t most) {
    if (s.empty() || s.size() > most) return false;
    for (wchar_t c : s) if (c < 32 || c == 127) return false;
    return true;
}
bool IsId(std::wstring_view s) {
    if (s.empty() || s.size() > 32) return false;
    for (wchar_t c : s) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    return s.front() != L'-' && s.back() != L'-';
}
bool Ends(std::wstring_view s, std::wstring_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (towlower(s[s.size() - suffix.size() + i]) != suffix[i]) return false;
    return true;
}
bool DeviceName(std::wstring_view stem) {
    std::wstring s;
    for (wchar_t c : stem) s += (wchar_t)towlower(c);
    if (s == L"con" || s == L"prn" || s == L"aux" || s == L"nul") return true;
    return s.size() == 4 && (s.rfind(L"com", 0) == 0 || s.rfind(L"lpt", 0) == 0) && s[3] >= L'1' && s[3] <= L'9';
}
// A file inside a package: up to four segments of letters, digits, '.', '_' and '-'; no segment
// starts with a dot, so no "..", no drive letters, no device names.
bool PackagePath(std::wstring_view path) {
    if (path.empty() || path.size() > 128) return false;
    size_t segments = 0, start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L'/', start);
        if (end == std::wstring_view::npos) end = path.size();
        const auto seg = path.substr(start, end - start);
        if (seg.empty() || seg.size() > 64 || seg[0] == L'.' || ++segments > 4) return false;
        for (wchar_t c : seg)
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-'))
                return false;
        if (DeviceName(seg.substr(0, seg.find(L'.')))) return false;
        if (end == path.size()) break;
        start = end + 1;
    }
    return true;
}
bool PackageExtension(AssetKind kind, std::wstring_view f) {
    switch (kind) {
    // A .tex is a picture already turned into a texture, as a scene exported from the library has it.
    case AssetKind::Image: case AssetKind::Mask: case AssetKind::Frames:
        return Ends(f, L".png") || Ends(f, L".jpg") || Ends(f, L".jpeg") || Ends(f, L".tex");
    case AssetKind::Video: return Ends(f, L".mp4") || Ends(f, L".mov") || Ends(f, L".mkv") || Ends(f, L".webm");
    case AssetKind::Lottie: return Ends(f, L".json");
    default: return Ends(f, L".hlsl");
    }
}
std::wstring StoredName(const Asset &a, size_t frame) {
    switch (a.kind) {
    case AssetKind::Image: case AssetKind::Mask: return L"textures/" + a.id + L".tex";
    case AssetKind::Frames: return L"textures/" + a.id + L"-" + std::to_wstring(frame) + L".tex";
    case AssetKind::Video: return L"video.mp4";
    case AssetKind::Lottie: return L"lottie/" + a.id + L".json";
    default: return L"shaders/" + a.id + L".hlsl";
    }
}

uint32_t Hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}
double Unit(uint32_t seed, long long k) { return Hash(seed, (uint32_t)(k * 2654435761u)) / 4294967296.0; }

// A parsed `when`: a data name or a $setting, optionally compared with a word or value.
struct Condition {
    bool negate = false, setting = false;
    std::wstring name;
    int op = 0;  // 0: has a value / is on, 1: equals, 2: differs
    std::wstring word;
};
bool SplitCondition(const std::wstring &s, Condition *c) {
    if (s.empty() || s.size() > 96) return false;
    std::wstring rest = s;
    const size_t ne = rest.find(L"!="), eq = rest.find(L'=');
    if (ne != std::wstring::npos) { c->op = 2; c->word = rest.substr(ne + 2); rest = rest.substr(0, ne); }
    else if (eq != std::wstring::npos) { c->op = 1; c->word = rest.substr(eq + 1); rest = rest.substr(0, eq); }
    if (!rest.empty() && rest[0] == L'!') {
        if (c->op) return false;
        c->negate = true; rest = rest.substr(1);
    }
    if (!rest.empty() && rest[0] == L'$') { c->setting = true; rest = rest.substr(1); }
    c->name = rest;
    return !c->name.empty() && (c->op == 0 || !c->word.empty());
}

class Checker {
public:
    Document d;
    std::wstring error;
    size_t nodes = 0, effects = 0, emitters = 0, particles = 0, order = 0;
    std::set<std::wstring> ids;

    bool Fail(int line, const std::wstring &s) { error = Format(L"line %d: %s", line, s.c_str()); return false; }
    bool Fail(const xml::Element &e, const std::wstring &s) { return Fail(e.line, s); }

    const Setting *FindSetting(const std::wstring &id) const { return d.FindSetting(id); }

    bool Literal(const Prop &p, const std::wstring &v, const std::vector<AssetKind> &kinds) const {
        double n = 0;
        switch (p.type) {
        case Type::Number: return Number(v, &n) && n >= p.lo && n <= p.hi;
        case Type::Integer: return Number(v, &n) && n >= p.lo && n <= p.hi && n == std::floor(n);
        case Type::Bool: return v == L"true" || v == L"false";
        case Type::Enum: return InWords(p.words, v);
        case Type::Color: return Color(v);
        case Type::Colors: return Stops(v);
        case Type::Pair: {
            const auto w = Words(v);
            double a = 0, b = 0;
            return w.size() == 2 && Number(w[0], &a) && Number(w[1], &b) && a >= p.lo && a <= p.hi && b >= p.lo && b <= p.hi;
        }
        case Type::Range: {
            const size_t dash = v.find(L'-', 1);
            double a = 0, b = 0;
            if (dash == std::wstring::npos) return Number(v, &a) && a >= p.lo && a <= p.hi;
            return Number(std::wstring_view(v).substr(0, dash), &a) && Number(std::wstring_view(v).substr(dash + 1), &b) &&
                   a >= p.lo && b <= p.hi && a <= b;
        }
        case Type::Asset: {
            const Asset *a = d.FindAsset(v);
            return a && std::find(kinds.begin(), kinds.end(), a->kind) != kinds.end();
        }
        case Type::Vector: return !Components(v).empty() || v == L"true" || v == L"false";
        }
        return false;
    }
    // A value: a literal of the attribute's type, or a $setting every value of which suits it.
    bool Value(int line, const Prop &p, const std::wstring &v, const std::vector<AssetKind> &kinds = {}) {
        if (v.empty() || v[0] != L'$') {
            if (Literal(p, v, kinds)) return true;
            return Fail(line, Format(L"%s: invalid value \"%s\"", p.name, v.c_str()));
        }
        const Setting *s = FindSetting(v.substr(1));
        if (!s) return Fail(line, L"unknown setting " + v);
        if (s->image) {
            if (p.type == Type::Asset && std::find(kinds.begin(), kinds.end(), AssetKind::Image) != kinds.end()) return true;
            return Fail(line, Format(L"%s: an image setting can only choose a layer's source", p.name));
        }
        const auto &b = s->base;
        switch (b.kind) {
        case skin::SettingKind::Number:
            if ((p.type == Type::Number || p.type == Type::Vector) && b.min >= p.lo && b.max <= p.hi) return true;
            if (p.type == Type::Vector) return true;
            break;
        case skin::SettingKind::Color:
            if (p.type == Type::Color || p.type == Type::Vector) return true;
            break;
        case skin::SettingKind::Colors:
            if (p.type == Type::Colors) return true;
            break;
        case skin::SettingKind::Choice: case skin::SettingKind::Toggle: {
            for (const auto &o : b.options) if (!Literal(p, o.value, kinds)) return Fail(line, Format(L"%s: setting %s has a value that does not suit it", p.name, v.c_str()));
            return true;
        }
        default: break;
        }
        return Fail(line, Format(L"%s: setting %s has an unsuitable kind or range", p.name, v.c_str()));
    }

    bool CheckCondition(int line, const std::wstring &s, bool settingsOnly) {
        Condition c;
        if (!SplitCondition(s, &c)) return Fail(line, L"invalid condition " + s);
        if (c.setting) {
            const Setting *st = FindSetting(c.name);
            if (!st || st->image) return Fail(line, L"condition needs a toggle or choice setting: " + s);
            if (st->base.kind == skin::SettingKind::Toggle) {
                if (c.op && c.word != L"on" && c.word != L"off") return Fail(line, L"a toggle is on or off: " + s);
                return true;
            }
            if (st->base.kind != skin::SettingKind::Choice || !c.op) return Fail(line, L"a choice is compared with one of its values: " + s);
            for (const auto &o : st->base.options) if (o.value == c.word) return true;
            return Fail(line, L"not a value of the setting: " + s);
        }
        if (settingsOnly) return Fail(line, L"a setting can only depend on other settings: " + s);
        const DataName *dn = FindData(c.name);
        if (!dn) return Fail(line, L"unknown data " + c.name);
        if (c.op && (!dn->words || !InWords(dn->words, c.word))) return Fail(line, L"not a word of the data: " + s);
        return true;
    }

    bool Settings(const xml::Element &e) {
        xml::Element component{L"component", {{L"format", L"1"}, {L"name", d.name}}, {}, e.line};
        xml::Element flat{L"settings", {}, {}, e.line};
        std::vector<Setting> found;
        auto take = [&](const xml::Element &s, const std::wstring &group) -> bool {
            Setting st; st.group = group;
            const auto *id = s.Find(L"id");
            if (!id || !IsId(*id)) return Fail(s, L"a setting needs an id");
            if (s.name == L"image") {
                const auto *label = s.Find(L"label"), *def = s.Find(L"default");
                for (const auto &[k, v] : s.attributes)
                    if (k != L"id" && k != L"label" && k != L"detail" && k != L"default" && k != L"when") return Fail(s, L"unknown attribute " + k);
                if (!label || !Label(*label, 40) || !def || !s.children.empty()) return Fail(s, L"an image setting needs a label and a default image");
                st.image = true; st.base.id = *id; st.base.label = *label; st.base.fallback = *def;
                if (const auto *detail = s.Find(L"detail")) { if (!Label(*detail, 80)) return Fail(s, L"invalid detail"); st.base.detail = *detail; }
            } else {
                xml::Element copy = s;
                copy.attributes.erase(std::remove_if(copy.attributes.begin(), copy.attributes.end(), [](const auto &a) { return a.first == L"when"; }), copy.attributes.end());
                // In a scene a toggle is mostly a condition, so its bound values default to true and false.
                if (copy.name == L"toggle") {
                    if (!copy.Find(L"on")) copy.attributes.emplace_back(L"on", L"true");
                    if (!copy.Find(L"off")) copy.attributes.emplace_back(L"off", L"false");
                }
                flat.children.push_back(std::move(copy));
                st.base.id = *id;
            }
            if (const auto *w = s.Find(L"when")) st.when = *w;
            found.push_back(std::move(st));
            return true;
        };
        for (const auto &s : e.children) {
            if (s.name == L"group") {
                const auto *label = s.Find(L"label");
                if (s.attributes.size() != 1 || !label || !Label(*label, 40)) return Fail(s, L"a group has only a label");
                for (const auto &c : s.children) {
                    if (c.name == L"group") return Fail(c, L"groups do not nest");
                    if (!take(c, *label)) return false;
                }
            } else if (!take(s, L"")) return false;
        }
        if (found.size() > kMaxSettings) return Fail(e, L"too many settings");
        component.children.push_back(std::move(flat));
        component.children.push_back({L"panel", {}, {{L"line", {}, {{L"text", {{L"value", L"x"}}, {}, e.line}}, e.line}}, e.line});
        skin::Skin parsed;
        std::wstring why;
        if (!skin::Parse(xml::Write(component), &parsed, &why)) { error = why; return false; }
        std::set<std::wstring> seen;
        for (auto &st : found) {
            if (!seen.insert(st.base.id).second) return Fail(e, L"repeated setting " + st.base.id);
            if (!st.image) {
                const skin::Setting *ps = parsed.Find(st.base.id);
                if (!ps || ps->kind == skin::SettingKind::Font || ps->kind == skin::SettingKind::Date)
                    return Fail(e, L"scenes take toggle, number, choice, color, colors and image settings: " + st.base.id);
                st.base = *ps;
            }
        }
        d.settings = std::move(found);
        for (const auto &st : d.settings)
            if (!st.when.empty()) {
                Condition c;
                if (!CheckCondition(e.line, st.when, true)) return false;
                SplitCondition(st.when, &c);
                if (c.name == st.base.id) return Fail(e, L"a setting cannot depend on itself: " + st.base.id);
            }
        return true;
    }

    bool Assets(const xml::Element &e) {
        size_t images = 0, videos = 0, lotties = 0, shaders = 0;
        for (const auto &a : e.children) {
            Asset asset; asset.line = a.line;
            const auto *id = a.Find(L"id");
            if (!id || !IsId(*id) || d.FindAsset(*id)) return Fail(a, L"an asset needs a unique id");
            asset.id = *id;
            if (a.name == L"image") asset.kind = AssetKind::Image;
            else if (a.name == L"mask") asset.kind = AssetKind::Mask;
            else if (a.name == L"frames") asset.kind = AssetKind::Frames;
            else if (a.name == L"video") asset.kind = AssetKind::Video;
            else if (a.name == L"lottie") asset.kind = AssetKind::Lottie;
            else if (a.name == L"shader") {
                const auto *kind = a.Find(L"kind");
                if (!kind || (*kind != L"layer" && *kind != L"effect")) return Fail(a, L"a shader's kind is layer or effect");
                asset.kind = *kind == L"layer" ? AssetKind::LayerShader : AssetKind::EffectShader;
            } else return Fail(a, L"unknown asset " + a.name);
            for (const auto &[k, v] : a.attributes) {
                if (k == L"id" || (k == L"kind" && a.name == L"shader")) continue;
                if (k == L"file" && asset.kind != AssetKind::Frames) asset.files.push_back(v);
                else if (k == L"exact" && (asset.kind == AssetKind::Image || asset.kind == AssetKind::Frames)) {
                    if (v != L"true" && v != L"false") return Fail(a, L"exact is true or false");
                    asset.exact = v == L"true";
                } else return Fail(a, L"unknown attribute " + k);
            }
            if (asset.kind == AssetKind::Frames) {
                for (const auto &f : a.children) {
                    const auto *file = f.Find(L"file");
                    if (f.name != L"frame" || f.attributes.size() != 1 || !file || !f.children.empty()) return Fail(f, L"a frame has only a file");
                    asset.files.push_back(*file);
                }
                if (asset.files.empty() || asset.files.size() > kMaxFrames) return Fail(a, L"a frame sequence has 1 to 120 frames");
            } else if (!a.children.empty() || asset.files.size() != 1) return Fail(a, L"an asset needs one file");
            for (size_t i = 0; i < asset.files.size(); ++i) {
                const auto &f = asset.files[i];
                const bool good = d.mode == Mode::Package ? PackagePath(f) && PackageExtension(asset.kind, f) : f == StoredName(asset, i);
                if (!good) return Fail(a, L"invalid file name " + f);
            }
            switch (asset.kind) {
            case AssetKind::Video: if (++videos > kMaxVideos) return Fail(a, L"a scene has at most one video"); break;
            case AssetKind::Lottie: if (++lotties > kMaxLotties) return Fail(a, L"too many Lottie animations"); break;
            case AssetKind::LayerShader: case AssetKind::EffectShader: if (++shaders > kMaxShaders) return Fail(a, L"too many shaders"); break;
            default: if (++images > kMaxImages) return Fail(a, L"too many images"); break;
            }
            d.assets.push_back(std::move(asset));
        }
        return true;
    }

    // What a motion's property is: its type and range. Returns false when the node has no such
    // animated property.
    bool Target(const Node &n, const std::wstring &property, Prop *out) {
        const size_t dot = property.find(L'.');
        if (dot != std::wstring::npos) {
            const std::wstring owner = property.substr(0, dot), name = property.substr(dot + 1);
            if (owner == L"param" || owner == L"slot") {
                for (const auto &[k, v] : n.params) if (k == name) { *out = {L"value", Type::Vector, -kBig, kBig, nullptr, true, L""}; return true; }
                return false;
            }
            for (const auto &e : n.effects) {
                if (e.id != owner) continue;
                if (!e.source.empty()) {
                    for (const auto &[k, v] : e.params) if (k == name) { *out = {L"value", Type::Vector, -kBig, kBig, nullptr, true, L""}; return true; }
                    return false;
                }
                for (const auto &p : FindEffect(e.type)->props) if (name == p.name && p.animated) { *out = p; return true; }
            }
            return false;
        }
        auto look = [&](const Prop *begin, const Prop *end) {
            for (const Prop *p = begin; p != end; ++p) if (property == p->name && p->animated) { *out = *p; return true; }
            return false;
        };
        if (look(std::begin(kCommon), std::end(kCommon))) return true;
        if (n.kind == NodeKind::Layer) return look(std::begin(kLayer), std::end(kLayer));
        if (n.kind == NodeKind::Fill) return look(std::begin(kFill), std::end(kFill));
        if (n.kind == NodeKind::Particles) return look(std::begin(kParticles), std::end(kParticles));
        return false;
    }

    bool ReadMotion(const xml::Element &e, const Node *node, std::vector<Motion> *out) {
        Motion m; m.line = e.line;
        if (e.name == L"keys") m.kind = MotionKind::Keys;
        else if (e.name == L"wave") m.kind = MotionKind::Wave;
        else if (e.name == L"wander") m.kind = MotionKind::Wander;
        else m.kind = MotionKind::Drive;
        const auto *property = e.Find(L"property");
        if (!property) return Fail(e, L"a motion needs a property");
        m.property = *property;
        Prop target{};
        if (node) { if (!Target(*node, m.property, &target)) return Fail(e, L"not an animated property: " + m.property); }
        else {
            if (m.property != L"x" && m.property != L"y" && m.property != L"scale" && m.property != L"rotation")
                return Fail(e, L"the camera moves x, y, scale and rotation");
            target = {L"camera", Type::Number, m.property == L"scale" ? 0.1 : -kBig, m.property == L"scale" ? 10 : kBig, nullptr, true, L""};
        }
        const bool numeric = target.type == Type::Number || target.type == Type::Vector;
        auto number = [&](const wchar_t *name, double lo, double hi, double *value) -> bool {
            const auto *v = e.Find(name);
            if (!v) return true;
            if (!Number(*v, value) || *value < lo || *value > hi) return Fail(e, Format(L"%s: invalid value", name));
            return true;
        };
        const Prop amount{L"amount", Type::Number, -kBig, kBig, nullptr, true, L""}, period{L"period", Type::Number, 0.05, 3600, nullptr, true, L""},
            phase{L"phase", Type::Number, 0, 1, nullptr, true, L""};
        for (const auto &[k, v] : e.attributes) {
            if (k == L"property") continue;
            bool known = false;
            switch (m.kind) {
            case MotionKind::Keys:
                if (k == L"duration") { known = true; if (!number(L"duration", 0.01, 3600, &m.duration)) return false; }
                else if (k == L"delay") { known = true; if (!number(L"delay", 0, 3600, &m.delay)) return false; }
                else if (k == L"repeat") { known = true; const int i = IndexOf(kRepeats, v); if (i < 0) return Fail(e, L"invalid repeat"); m.repeat = (Repeat)i; }
                else if (k == L"gap") {
                    known = true;
                    const Prop gap{L"gap", Type::Range, 0, 3600, nullptr, false, L""};
                    if (!Literal(gap, v, {})) return Fail(e, L"invalid gap");
                    const size_t dash = v.find(L'-', 1);
                    Number(std::wstring_view(v).substr(0, dash), &m.gapMin);
                    m.gapMax = m.gapMin;
                    if (dash != std::wstring::npos) Number(std::wstring_view(v).substr(dash + 1), &m.gapMax);
                }
                break;
            case MotionKind::Wave: case MotionKind::Wander:
                if (k == L"amplitude") { known = true; if (!Value(e.line, amount, v)) return false; m.amplitude = v; }
                else if (k == L"period") { known = true; if (!Value(e.line, period, v)) return false; m.period = v; }
                else if (k == L"phase" && m.kind == MotionKind::Wave) { known = true; if (!Value(e.line, phase, v)) return false; m.phase = v; }
                break;
            case MotionKind::Drive:
                if (k == L"by") { known = true; m.by = v; }
                else if (k == L"from" || k == L"to") { known = true; if (!Value(e.line, target, v)) return false; (k == L"from" ? m.from : m.to) = v; }
                else if (k == L"range") {
                    known = true;
                    const auto w = Words(v);
                    if (w.size() != 2 || !Number(w[0], &m.inLow) || !Number(w[1], &m.inHigh) || m.inLow == m.inHigh) return Fail(e, L"invalid range");
                }
                break;
            }
            if (!known) return Fail(e, L"unknown attribute " + k);
        }
        if (m.kind == MotionKind::Keys) {
            if (m.duration <= 0) return Fail(e, L"keys need a duration");
            if (e.children.empty() || e.children.size() > kMaxKeys) return Fail(e, L"keys hold 1 to 64 keys");
            double last = -1;
            for (const auto &k : e.children) {
                Key key;
                const auto *at = k.Find(L"at"), *value = k.Find(L"value");
                if (k.name != L"key" || !at || !value || !k.children.empty()) return Fail(k, L"a key needs at and value");
                if (!Number(*at, &key.at) || key.at < 0 || key.at > m.duration || key.at <= last) return Fail(k, L"keys go in order within the duration");
                last = key.at;
                if (!Value(k.line, target, *value)) return false;
                key.value = *value;
                for (const auto &[a, v] : k.attributes) {
                    if (a == L"at" || a == L"value") continue;
                    if (a == L"ease") { const int i = IndexOf(kEases, v); if (i < 0) return Fail(k, L"invalid ease"); key.ease = (Ease)i; }
                    else if (a == L"bezier") {
                        const auto w = Words(v);
                        double n[4];
                        if (w.size() != 4) return Fail(k, L"bezier takes four numbers");
                        for (int i = 0; i < 4; ++i) if (!Number(w[i], &n[i]) || ((i % 2 == 0) && (n[i] < 0 || n[i] > 1)) || std::abs(n[i]) > 10) return Fail(k, L"invalid bezier");
                        for (int i = 0; i < 4; ++i) key.bezier[i] = (float)n[i];
                    } else return Fail(k, L"unknown attribute " + a);
                }
                if ((key.ease == Ease::Bezier) != (k.Find(L"bezier") != nullptr)) return Fail(k, L"ease=\"bezier\" goes with a bezier attribute");
                m.keys.push_back(std::move(key));
            }
        } else if (!e.children.empty()) return Fail(e, L"only keys hold children");
        if ((m.kind == MotionKind::Wave || m.kind == MotionKind::Wander) && (!numeric || m.amplitude.empty() || m.period.empty()))
            return Fail(e, L"wave and wander move a number by an amplitude over a period");
        if (m.kind == MotionKind::Drive) {
            if (m.by.empty() || m.from.empty() || m.to.empty()) return Fail(e, L"drive needs by, from and to");
            if (m.by[0] == L'$') {
                const Setting *s = FindSetting(m.by.substr(1));
                if (!s || s->image || s->base.kind != skin::SettingKind::Number) return Fail(e, L"drive by a number setting or a number data");
                if (!e.Find(L"range")) { m.inLow = s->base.min; m.inHigh = s->base.max; }
            } else {
                const DataName *dn = FindData(m.by);
                if (!dn || dn->words) return Fail(e, L"drive by a number setting or a number data");
                if (!e.Find(L"range") && m.by == L"date.month") { m.inLow = 1; m.inHigh = 12; }
            }
        }
        for (const auto &other : *out)
            if (other.property == m.property && (other.kind == MotionKind::Keys || other.kind == MotionKind::Drive) &&
                (m.kind == MotionKind::Keys || m.kind == MotionKind::Drive))
                return Fail(e, L"only one keys or drive sets a property: " + m.property);
        out->push_back(std::move(m));
        return true;
    }

    bool ReadEffect(const xml::Element &e, Node *n) {
        Effect fx; fx.line = e.line;
        const auto *type = e.Find(L"type"), *source = e.Find(L"source");
        if ((type != nullptr) == (source != nullptr)) return Fail(e, L"an effect has a type or a source");
        const EffectSpec *spec = type ? FindEffect(*type) : nullptr;
        if (type && !spec) return Fail(e, L"unknown effect " + *type);
        if (source) {
            const Asset *a = d.FindAsset(*source);
            if (!a || a->kind != AssetKind::EffectShader) return Fail(e, L"an effect's source is an effect shader");
            fx.source = *source;
        } else fx.type = *type;
        const Prop mask{L"mask", Type::Asset, 0, 0, nullptr, false, L""};
        for (const auto &[k, v] : e.attributes) {
            if (k == L"type" || k == L"source") continue;
            if (k == L"id") { if (!IsId(v) || !ids.insert(v).second) return Fail(e, L"ids must be unique"); fx.id = v; }
            else if (k == L"mask") { if (!Value(e.line, mask, v, {AssetKind::Mask})) return false; fx.mask = v; }
            else if (k == L"when") { if (!CheckCondition(e.line, v, false)) return false; fx.when = v; }
            else if (spec) {
                const Prop *p = nullptr;
                for (const auto &q : spec->props) if (k == q.name) p = &q;
                if (!p) return Fail(e, Format(L"%s does not take %s", spec->type, k.c_str()));
                if (!Value(e.line, *p, v)) return false;
                fx.params.emplace_back(k, v);
            } else return Fail(e, L"unknown attribute " + k);
        }
        if (source) {
            const Prop vec{L"param", Type::Vector, -kBig, kBig, nullptr, true, L""};
            std::set<std::wstring> slots;
            for (const auto &c : e.children) {
                const auto *slot = c.Find(L"slot"), *value = c.Find(L"value");
                long long index = -1;
                if (c.name != L"param" || c.attributes.size() != 2 || !slot || !value || !c.children.empty() ||
                    !ParseInt(*slot, &index) || index < 0 || index > 15 || *slot != std::to_wstring(index) || !slots.insert(*slot).second)
                    return Fail(c, L"a param has a slot 0-15 and a value");
                if (!Value(c.line, vec, *value)) return false;
                fx.params.emplace_back(*slot, *value);
            }
        } else if (!e.children.empty()) return Fail(e, L"a built-in effect holds no children");
        n->effects.push_back(std::move(fx));
        return ++effects <= kMaxEffects || Fail(e, L"too many effects");
    }

    bool ReadNode(const xml::Element &e, size_t depth, Node *out) {
        Node n; n.line = e.line;
        if (e.name == L"layer") n.kind = NodeKind::Layer;
        else if (e.name == L"fill") n.kind = NodeKind::Fill;
        else if (e.name == L"particles") n.kind = NodeKind::Particles;
        else if (e.name == L"group") n.kind = NodeKind::Group;
        else return Fail(e, L"unknown element " + e.name);
        if (++nodes > kMaxNodes) return Fail(e, L"too many nodes");
        if (n.kind == NodeKind::Group && depth >= kMaxNesting) return Fail(e, L"groups nest at most four deep");
        if (n.kind == NodeKind::Particles && ++emitters > kMaxEmitters) return Fail(e, L"too many particle emitters");
        const Prop *own = nullptr, *ownEnd = nullptr;
        if (n.kind == NodeKind::Layer) { own = std::begin(kLayer); ownEnd = std::end(kLayer); }
        else if (n.kind == NodeKind::Fill) { own = std::begin(kFill); ownEnd = std::end(kFill); }
        else if (n.kind == NodeKind::Particles) { own = std::begin(kParticles); ownEnd = std::end(kParticles); }
        std::vector<AssetKind> sources;
        if (n.kind == NodeKind::Layer) sources = {AssetKind::Image, AssetKind::Frames, AssetKind::Video, AssetKind::Lottie, AssetKind::LayerShader};
        else if (n.kind == NodeKind::Particles) sources = {AssetKind::Image, AssetKind::Frames};
        for (const auto &[k, v] : e.attributes) {
            if (k == L"id") { if (!IsId(v) || !ids.insert(v).second) return Fail(e, L"ids must be unique"); n.id = v; continue; }
            if (k == L"when") { if (!CheckCondition(e.line, v, false)) return false; n.attributes.emplace_back(k, v); continue; }
            const Prop *p = nullptr;
            for (const Prop &q : kCommon) if (k == q.name) p = &q;
            for (const Prop *q = own; q && q != ownEnd; ++q) if (k == q->name) p = q;
            if (!p) return Fail(e, Format(L"%s does not take %s", e.name.c_str(), k.c_str()));
            if (!Value(e.line, *p, v, k == L"mask" ? std::vector<AssetKind>{AssetKind::Mask} : sources)) return false;
            n.attributes.emplace_back(k, v);
        }
        if (n.kind == NodeKind::Layer && !n.Find(L"source")) return Fail(e, L"a layer needs a source");
        if (n.kind == NodeKind::Particles) {
            double most = 2000;
            if (const auto *m = n.Find(L"max")) Number(*m, &most);
            particles += (size_t)most;
            if (particles > kMaxParticles) return Fail(e, L"too many particles in all");
        }
        // Children: effects first is not required; nodes only inside groups.
        std::set<std::wstring> slots;
        for (const auto &c : e.children) {
            if (c.name == L"effect") {
                if (n.effects.size() == kMaxEffectsPerNode) return Fail(c, L"too many effects on one node");
                if (!ReadEffect(c, &n)) return false;
            } else if (c.name == L"param" || c.name == L"slot") {
                const auto *src = n.Find(L"source");
                const Asset *a = src && (*src)[0] != L'$' ? d.FindAsset(*src) : nullptr;
                const bool param = c.name == L"param" && a && a->kind == AssetKind::LayerShader;
                const bool slot = c.name == L"slot" && a && a->kind == AssetKind::Lottie;
                const auto *key = c.Find(param ? L"slot" : L"id"), *value = c.Find(L"value");
                long long index = -1;
                if ((!param && !slot) || c.attributes.size() != 2 || !key || !value || !c.children.empty() ||
                    (param ? !ParseInt(*key, &index) || index < 0 || index > 15 || *key != std::to_wstring(index) : !IsId(*key)) ||
                    !slots.insert(*key).second)
                    return Fail(c, L"a shader layer takes <param slot value>, a Lottie layer <slot id value>");
                const Prop vec{L"value", Type::Vector, -kBig, kBig, nullptr, true, L""};
                if (!Value(c.line, vec, *value)) return false;
                n.params.emplace_back(*key, *value);
            } else if (c.name == L"keys" || c.name == L"wave" || c.name == L"wander" || c.name == L"drive") {
                continue;  // read once the effects and params they may target are known
            } else if (n.kind == NodeKind::Group) {
                Node child;
                if (!ReadNode(c, depth + 1, &child)) return false;
                n.children.push_back(std::move(child));
            } else return Fail(c, Format(L"%s cannot hold %s", e.name.c_str(), c.name.c_str()));
        }
        for (const auto &c : e.children)
            if (c.name == L"keys" || c.name == L"wave" || c.name == L"wander" || c.name == L"drive") {
                if (n.motions.size() == kMaxMotionsPerNode) return Fail(c, L"too many motions on one node");
                if (!ReadMotion(c, &n, &n.motions)) return false;
            }
        *out = std::move(n);
        return true;
    }

    bool Run() {
        const auto &root = d.xml;
        const auto *format = root.Find(L"format");
        if (root.name != L"scene" || !format) return Fail(root, L"expected a scene");
        if (*format != L"2") {
            double v = 0;
            if (Number(*format, &v) && v > kFormat) return Fail(root, L"this scene needs a newer AnimeLogon");
            return Fail(root, L"expected scene format 2");
        }
        double w = 1920, h = 1080;
        for (const auto &[k, v] : root.attributes) {
            if (k == L"format") continue;
            if (k == L"name") { if (!Label(v, 64)) return Fail(root, L"invalid name"); d.name = v; }
            else if (k == L"author") { if (!v.empty() && !Label(v, 64)) return Fail(root, L"invalid author"); d.author = v; }
            else if (k == L"width" || k == L"height") {
                double n = 0;
                if (!Number(v, &n) || n < 16 || n > 16384 || n != std::floor(n)) return Fail(root, L"invalid stage size");
                (k == L"width" ? w : h) = n;
            } else if (k == L"still") { if (!Number(v, &d.still) || d.still < 0 || d.still > 3600) return Fail(root, L"invalid still"); }
            else return Fail(root, L"unknown attribute " + k);
        }
        if (d.name.empty()) return Fail(root, L"a scene needs a name");
        d.width = (int)w; d.height = (int)h;
        size_t settings = 0, assets = 0, cameras = 0;
        for (const auto &e : root.children) {
            if (e.name == L"settings") { if (++settings > 1 || !e.attributes.empty() || !Settings(e)) return error.empty() ? Fail(e, L"one <settings>") : false; }
            else if (e.name == L"assets") { if (++assets > 1 || !e.attributes.empty() || !Assets(e)) return error.empty() ? Fail(e, L"one <assets>") : false; }
        }
        for (const auto &st : d.settings)
            if (st.image) {
                const Asset *a = d.FindAsset(st.base.fallback);
                if (!a || a->kind != AssetKind::Image) return Fail(root, L"an image setting's default is an image asset: " + st.base.id);
            }
        bool any = false;
        for (const auto &e : root.children) {
            if (e.name == L"settings" || e.name == L"assets") continue;
            if (e.name == L"camera") {
                if (++cameras > 1 || !e.attributes.empty()) return Fail(e, L"one <camera> without attributes");
                for (const auto &c : e.children) {
                    if (c.name != L"keys" && c.name != L"wave" && c.name != L"wander" && c.name != L"drive") return Fail(c, L"the camera holds motions");
                    if (!ReadMotion(c, nullptr, &d.camera)) return false;
                }
                continue;
            }
            Node n;
            if (!ReadNode(e, 0, &n)) return false;
            d.nodes.push_back(std::move(n));
            any = true;
        }
        if (!any) return Fail(root, L"a scene draws at least one node");
        return true;
    }
};

// --- evaluation -------------------------------------------------------------------------------

std::wstring Resolve(const Document &d, const Values &values, const std::wstring &v) {
    if (v.empty() || v[0] != L'$') return v;
    const Setting *s = d.FindSetting(v.substr(1));
    if (!s) return {};
    const std::wstring chosen = ValueOf(d, values, s->base.id);
    // The picture the user chose comes to the renderer as its own file (ChosenName); the node keeps
    // the default's box.
    if (s->image) return s->base.fallback;
    if (s->base.kind == skin::SettingKind::Toggle) return chosen == L"on" ? s->base.options[0].value : s->base.options[1].value;
    return chosen;
}
std::array<float, 4> Vec(const std::wstring &literal) {
    std::array<float, 4> out{};
    const auto c = Components(literal);
    std::copy(c.begin(), c.begin() + std::min<size_t>(4, c.size()), out.begin());
    return out;
}
double DataNumber(const Inputs &in, const std::wstring &name, bool *known) {
    *known = true;
    if (name == L"time") return in.time;
    if (name == L"clock.day") return in.day;
    if (name == L"wake") return in.wake;
    if (name == L"date.month") return in.month;
    if (name == L"battery.level") { *known = in.battery >= 0; return std::max(0.0, in.battery); }
    if (name == L"media.progress") { *known = in.progress >= 0; return std::max(0.0, in.progress); }
    if (name == L"audio.level") return in.audio[0];
    if (name == L"audio.bass") return in.audio[1];
    if (name == L"audio.mid") return in.audio[2];
    if (name == L"audio.treble") return in.audio[3];
    *known = false;
    return 0;
}
std::wstring DataWord(const Inputs &in, const std::wstring &name) {
    if (name == L"clock.period") return Period(in);
    if (name == L"date.season") return Season(in);
    if (name == L"battery.state") return in.batteryState;
    if (name == L"power.source") return in.powerSource;
    if (name == L"media.state") return in.mediaState;
    return {};
}
bool Holds(const Document &d, const Values &values, const Inputs &in, const std::wstring &when) {
    if (when.empty()) return true;
    Condition c;
    if (!SplitCondition(when, &c)) return false;
    bool result = false;
    if (c.setting) {
        const std::wstring v = ValueOf(d, values, c.name);
        result = c.op == 0 ? v == L"on" : (c.op == 1) == (v == c.word);
    } else if (const DataName *dn = FindData(c.name); dn && dn->words) {
        const std::wstring w = DataWord(in, c.name);
        result = c.op == 0 ? w != L"none" && !w.empty() : (c.op == 1) == (w == c.word);
    } else {
        bool known = false;
        DataNumber(in, c.name, &known);
        result = known;
    }
    return c.negate ? !result : result;
}

double Ease01(const Key &k, double u) {
    switch (k.ease) {
    case Ease::Linear: return u;
    case Ease::Smooth: return u * u * (3 - 2 * u);
    case Ease::In: return u * u;
    case Ease::Out: return 1 - (1 - u) * (1 - u);
    case Ease::Step: return u < 1 ? 0 : 1;
    case Ease::Bezier: {
        // Solve x(s) = u on the cubic from (0,0) to (1,1) through the two control points, then take y(s).
        const double x1 = k.bezier[0], y1 = k.bezier[1], x2 = k.bezier[2], y2 = k.bezier[3];
        auto bx = [&](double s) { return 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s; };
        auto by = [&](double s) { return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s; };
        double lo = 0, hi = 1, s = u;
        for (int i = 0; i < 40; ++i) { s = (lo + hi) / 2; if (bx(s) < u) lo = s; else hi = s; }
        return by(s);
    }
    }
    return u;
}

}  // namespace

// Where a run of `length` seconds is at time t. Without a gap it repeats as asked (bounce goes there
// and back); with a gap each run plays once (bounce: there and back) and then rests at its start for
// a pseudo-random time between gapMin and gapMax, worked out without stepping through earlier runs.
double Local(double t, double length, Repeat repeat, double delay, double gapMin, double gapMax, uint32_t seed) {
    t -= delay;
    if (t <= 0 || length <= 0) return 0;
    const double run = repeat == Repeat::Bounce ? 2 * length : length;
    auto fold = [&](double u) { return repeat == Repeat::Bounce && u > length ? 2 * length - u : u; };
    if (gapMax <= 0) {
        if (repeat == Repeat::Once) return std::min(t, length);
        return fold(std::fmod(t, run));
    }
    const double mid = (gapMin + gapMax) / 2, half = (gapMax - gapMin) / 2, slot = run + mid;
    const long long k = (long long)std::floor(t / slot);
    const double start = k * slot + half * Unit(seed, k);
    return t >= start && t < start + run ? fold(t - start) : 0;
}

namespace {
double Noise1(double x, uint32_t seed) {
    const double i = std::floor(x), f = x - i;
    const double a = Unit(seed, (long long)i), b = Unit(seed, (long long)i + 1);
    const double s = f * f * (3 - 2 * f);
    return (a + (b - a) * s) * 2 - 1;
}

struct Evaluator {
    const Document &d;
    const Values &values;
    const Inputs &in;
    uint32_t order = 0;

    double Num(const std::wstring &v) { const auto c = Components(Resolve(d, values, v)); return c.empty() ? 0 : c[0]; }

    // Applies every motion aimed at `property` to `value`.
    void Move(const std::vector<Motion> &motions, uint32_t salt, const std::wstring &property, std::array<float, 4> *value, size_t width) {
        for (size_t i = 0; i < motions.size(); ++i) {
            const Motion &m = motions[i];
            if (m.property != property) continue;
            const uint32_t seed = Hash(salt, (uint32_t)i + 1);
            switch (m.kind) {
            case MotionKind::Keys: {
                const double t = Local(in.time, m.duration, m.repeat, m.delay, m.gapMin, m.gapMax, seed);
                const Key *a = &m.keys.front(), *b = a;
                for (const Key &k : m.keys) { if (k.at <= t) a = &k; }
                b = a;
                for (const Key &k : m.keys) if (k.at > t) { b = &k; break; }
                const auto va = Vec(Resolve(d, values, a->value)), vb = Vec(Resolve(d, values, b->value));
                const double u = b == a || b->at <= a->at ? 0 : Ease01(*a, std::clamp((t - a->at) / (b->at - a->at), 0.0, 1.0));
                for (size_t c = 0; c < width; ++c) (*value)[c] = (float)(va[c] + (vb[c] - va[c]) * u);
                break;
            }
            case MotionKind::Drive: {
                double input = 0;
                bool known = true;
                if (m.by[0] == L'$') input = Num(m.by);
                else input = DataNumber(in, m.by, &known);
                const double u = known ? std::clamp((input - m.inLow) / (m.inHigh - m.inLow), 0.0, 1.0) : 0;
                const auto va = Vec(Resolve(d, values, m.from)), vb = Vec(Resolve(d, values, m.to));
                for (size_t c = 0; c < width; ++c) (*value)[c] = (float)(va[c] + (vb[c] - va[c]) * u);
                break;
            }
            case MotionKind::Wave: {
                const double period = std::max(0.05, Num(m.period));
                (*value)[0] += (float)(Num(m.amplitude) * std::sin(2 * kPi * (in.time / period + (m.phase.empty() ? 0 : Num(m.phase)))));
                break;
            }
            case MotionKind::Wander: {
                const double x = in.time / std::max(0.05, Num(m.period));
                (*value)[0] += (float)(Num(m.amplitude) * (0.7 * Noise1(x, seed) + 0.3 * Noise1(x * 2.3 + 17, seed ^ 0x5bd1e995u)));
                break;
            }
            }
        }
    }

    bool Place(const scenegraph::Node &n, Placed *out) {
        const uint32_t salt = Hash(0x51A7E000u, ++order);
        if (const auto *w = n.Find(L"when"); w && !Holds(d, values, in, *w)) {
            for (const auto &c : n.children) Skip(c);
            return false;
        }
        Placed p;
        p.node = &n;
        const Prop *own = nullptr, *ownEnd = nullptr;
        if (n.kind == NodeKind::Layer) { own = std::begin(kLayer); ownEnd = std::end(kLayer); }
        else if (n.kind == NodeKind::Fill) { own = std::begin(kFill); ownEnd = std::end(kFill); }
        else if (n.kind == NodeKind::Particles) { own = std::begin(kParticles); ownEnd = std::end(kParticles); }
        // A particle preset only stands in for what the element leaves out.
        const auto *presetText = n.kind == NodeKind::Particles ? n.Find(L"preset") : nullptr;
        const std::wstring preset = presetText ? Resolve(d, values, *presetText) : L"";
        auto take = [&](const Prop &prop) {
            const auto *written = n.Find(prop.name);
            const wchar_t *fallback = preset.empty() ? nullptr : PresetDefault(preset, prop.name);
            const std::wstring text = written ? Resolve(d, values, *written) : fallback ? fallback : prop.fallback;
            p.text[prop.name] = text;
            if (prop.type == Type::Number || prop.type == Type::Integer || prop.type == Type::Color || prop.type == Type::Pair) {
                auto v = Vec(text);
                if (prop.animated) Move(n.motions, salt, prop.name, &v, prop.type == Type::Color ? 4 : 1);
                if (prop.name == std::wstring(L"opacity")) v[0] = std::clamp(v[0], 0.f, 1.f);
                p.values[prop.name] = v;
            }
        };
        for (const Prop &prop : kCommon) take(prop);
        for (const Prop *q = own; q && q != ownEnd; ++q) take(*q);
        const Asset *sourceAsset = n.kind == NodeKind::Layer ? d.FindAsset(p.text[L"source"]) : nullptr;
        const std::wstring paramPrefix = sourceAsset && sourceAsset->kind == AssetKind::Lottie ? L"slot." : L"param.";
        for (const auto &[k, v] : n.params) {
            auto value = Vec(Resolve(d, values, v));
            Move(n.motions, salt, paramPrefix + k, &value, 4);
            p.values[paramPrefix + k] = value;
        }
        auto f = [&](const wchar_t *k) { return p.values[k][0]; };
        p.x = f(L"x"); p.y = f(L"y"); p.width = f(L"width"); p.height = f(L"height");
        p.pivotX = p.values[L"pivot"][0]; p.pivotY = p.values[L"pivot"][1];
        p.rotation = f(L"rotation"); p.opacity = f(L"opacity"); p.depth = f(L"depth");
        p.scaleX = f(L"scale") * f(L"scale-x"); p.scaleY = f(L"scale") * f(L"scale-y");
        p.anchor = std::max(0, IndexOf(kAnchors, p.text[L"anchor"]));
        p.space = p.text[L"space"] == L"screen" ? Space::Screen : Space::Stage;
        p.blend = (Blend)std::max(0, IndexOf(kBlends, p.text[L"blend"]));
        p.fit = (Fit)std::max(0, IndexOf(kFits, p.text[L"fit"]));
        p.full = p.text[L"full"] == L"true";
        p.mask = p.text[L"mask"];
        p.localTime = in.time;
        if (n.kind == NodeKind::Layer) {
            p.source = p.text[L"source"];
            if (const Asset *a = d.FindAsset(p.source); a && a->kind == AssetKind::Frames) {
                const double fps = std::max(1.0, (double)f(L"fps")), length = a->files.size() / fps;
                double gapMin = 0, gapMax = 0;
                const std::wstring gap = p.text[L"gap"];
                if (!gap.empty()) {
                    const size_t dash = gap.find(L'-', 1);
                    Number(std::wstring_view(gap).substr(0, dash), &gapMin);
                    gapMax = gapMin;
                    if (dash != std::wstring::npos) Number(std::wstring_view(gap).substr(dash + 1), &gapMax);
                }
                const Repeat repeat = (Repeat)std::max(0, IndexOf(kRepeats, p.text[L"repeat"]));
                const double local = Local(in.time, length, repeat, 0, gapMin, gapMax, Hash(salt, 0xF4A3u));
                p.frame = std::clamp((int)std::floor(local * fps), 0, (int)a->files.size() - 1);
            }
        } else if (n.kind == NodeKind::Particles) p.source = p.text[L"source"];
        for (const Effect &fx : n.effects) {
            if (!Holds(d, values, in, fx.when)) continue;
            PlacedEffect pe;
            pe.effect = &fx;
            pe.mask = Resolve(d, values, fx.mask);
            if (const EffectSpec *spec = fx.type.empty() ? nullptr : FindEffect(fx.type)) {
                for (const Prop &prop : spec->props) {
                    std::wstring text = prop.fallback;
                    for (const auto &[k, v] : fx.params) if (k == prop.name) text = Resolve(d, values, v);
                    auto v = Vec(text);
                    if (prop.type == Type::Enum) v[0] = (float)std::max(0, IndexOf(prop.words, text));
                    if (prop.animated && !fx.id.empty()) Move(n.motions, salt, fx.id + L"." + prop.name, &v, prop.type == Type::Color ? 4 : 1);
                    pe.values[prop.name] = v;
                }
            } else {
                for (const auto &[k, v] : fx.params) {
                    auto value = Vec(Resolve(d, values, v));
                    if (!fx.id.empty()) Move(n.motions, salt, fx.id + L"." + k, &value, 4);
                    pe.values[k] = value;
                }
            }
            p.effects.push_back(std::move(pe));
        }
        for (const auto &c : n.children) {
            Placed child;
            if (Place(c, &child)) p.children.push_back(std::move(child));
        }
        *out = std::move(p);
        return true;
    }
    // Keeps the numbering of nodes the same whether or not a group is shown, so pseudo-random timing
    // does not shift when a condition changes.
    void Skip(const scenegraph::Node &n) { ++order; for (const auto &c : n.children) Skip(c); }
};

void SetDefault(xml::Element &settings, const std::wstring &id, const std::wstring &value) {
    for (auto &e : settings.children) {
        if (e.name == L"group") { SetDefault(e, id, value); continue; }
        const auto *eid = e.Find(L"id");
        if (!eid || *eid != id) continue;
        for (auto &a : e.attributes) if (a.first == L"default") a.second = value;
    }
}
}  // namespace

const std::wstring *Node::Find(std::wstring_view name) const {
    for (const auto &[k, v] : attributes) if (k == name) return &v;
    return nullptr;
}
const Asset *Document::FindAsset(std::wstring_view id) const {
    for (const auto &a : assets) if (a.id == id) return &a;
    return nullptr;
}
const Setting *Document::FindSetting(std::wstring_view id) const {
    for (const auto &s : settings) if (s.base.id == id) return &s;
    return nullptr;
}

bool Parse(std::string_view utf8, Mode mode, Document *out, std::wstring *why) {
    Checker c;
    c.d.mode = mode;
    xml::Limits limits;
    limits.bytes = kMaxTextBytes;
    limits.depth = 10;
    limits.elements = 4096;
    limits.attributes = 32;
    limits.value = 512;
    if (!xml::Parse(utf8, &c.d.xml, &c.error, limits) || !c.Run()) { if (why) *why = c.error; return false; }
    *out = std::move(c.d);
    return true;
}
std::string Write(const Document &d) { return xml::Write(d.xml); }

std::vector<std::pair<std::wstring, std::wstring>> StoredNames(const Document &d) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto &a : d.assets)
        for (size_t i = 0; i < a.files.size(); ++i) out.emplace_back(a.files[i], StoredName(a, i));
    return out;
}
bool Stored(const Document &package, Document *out, std::wstring *why) {
    xml::Element root = package.xml;
    for (auto &e : root.children) {
        if (e.name != L"assets") continue;
        for (auto &a : e.children) {
            const Asset *asset = package.FindAsset(*a.Find(L"id"));
            if (!asset) continue;
            if (asset->kind == AssetKind::Frames) {
                for (size_t i = 0; i < a.children.size(); ++i) a.children[i].attributes = {{L"file", StoredName(*asset, i)}};
            } else
                for (auto &[k, v] : a.attributes) if (k == L"file") v = StoredName(*asset, 0);
        }
    }
    return Parse(xml::Write(root), Mode::Stored, out, why);
}

bool Accepts(const Document &d, const std::wstring &id, const std::wstring &value) {
    const Setting *s = d.FindSetting(id);
    if (!s) return false;
    if (s->image) {
        if (value == s->base.fallback) return true;
        if (value.size() != 16) return false;
        for (wchar_t c : value) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
        return true;
    }
    return skin::IsValue(s->base, value);
}
std::wstring ValueOf(const Document &d, const Values &values, const std::wstring &id) {
    const Setting *s = d.FindSetting(id);
    if (!s) return {};
    const auto it = values.find(id);
    return it != values.end() && Accepts(d, id, it->second) ? it->second : s->base.fallback;
}
bool Shown(const Document &d, const Values &values, const Setting &s) {
    static const Inputs none;
    return Holds(d, values, none, s.when);
}
bool Bake(const Document &d, const Values &values, Document *out, std::wstring *why) {
    xml::Element root = d.xml;
    for (auto &e : root.children) {
        if (e.name != L"settings") continue;
        for (const auto &s : d.settings) {
            if (s.image) continue;
            const auto it = values.find(s.base.id);
            if (it != values.end() && Accepts(d, it->first, it->second)) SetDefault(e, s.base.id, it->second);
        }
    }
    return Parse(xml::Write(root), d.mode, out, why);
}

std::wstring Period(const Inputs &in) {
    const double h = in.day * 24;
    if (h >= 5 && h < 7) return L"dawn";
    if (h >= 7 && h < 17) return L"day";
    if (h >= 17 && h < 19) return L"dusk";
    return L"night";
}
std::wstring Season(const Inputs &in) {
    const int m = in.month;
    if (m >= 3 && m <= 5) return L"spring";
    if (m >= 6 && m <= 8) return L"summer";
    if (m >= 9 && m <= 11) return L"autumn";
    return L"winter";
}

Frame Evaluate(const Document &d, const Values &values, const Inputs &in) {
    Frame f;
    Evaluator ev{d, values, in};
    std::array<float, 4> x{}, y{}, scale{1}, rotation{};
    const uint32_t salt = Hash(0xCA3E1A00u, 0);
    ev.Move(d.camera, salt, L"x", &x, 1);
    ev.Move(d.camera, salt, L"y", &y, 1);
    ev.Move(d.camera, salt, L"scale", &scale, 1);
    ev.Move(d.camera, salt, L"rotation", &rotation, 1);
    f.cameraX = x[0]; f.cameraY = y[0]; f.cameraScale = std::max(0.1f, scale[0]); f.cameraRotation = rotation[0];
    for (const auto &n : d.nodes) {
        Placed p;
        if (ev.Place(n, &p)) f.nodes.push_back(std::move(p));
    }
    return f;
}

bool EffectMoves(const Effect &effect) { return !effect.source.empty() || TimeDriven(effect.type); }

bool ReadsSound(const Document &d) {
    const auto audio = [](const std::wstring &name) {
        const size_t at = name.find(L"audio.");
        return at == 0 || (at == 1 && name[0] == L'!');
    };
    for (const Asset &a : d.assets) if (a.kind == AssetKind::LayerShader || a.kind == AssetKind::EffectShader) return true;
    const auto drives = [&](const std::vector<Motion> &motions) {
        for (const Motion &m : motions) if (m.kind == MotionKind::Drive && audio(m.by)) return true;
        return false;
    };
    if (drives(d.camera)) return true;
    std::function<bool(const Node &)> reads = [&](const Node &n) {
        if (drives(n.motions)) return true;
        if (const auto *w = n.Find(L"when"); w && audio(*w)) return true;
        for (const Effect &e : n.effects) if (audio(e.when)) return true;
        for (const Node &c : n.children) if (reads(c)) return true;
        return false;
    };
    for (const Node &n : d.nodes) if (reads(n)) return true;
    return false;
}

bool Moves(const Document &d) {
    if (!d.camera.empty()) return true;
    std::function<bool(const Node &)> moves = [&](const Node &n) {
        if (!n.motions.empty() || n.kind == NodeKind::Particles) return true;
        if (n.kind == NodeKind::Layer) {
            const auto *src = n.Find(L"source");
            const Asset *a = src ? d.FindAsset(*src) : nullptr;
            if (a && (a->kind == AssetKind::Video || a->kind == AssetKind::Lottie || a->kind == AssetKind::LayerShader ||
                      (a->kind == AssetKind::Frames && a->files.size() > 1)))
                return true;
        }
        for (const auto &fx : n.effects) if (EffectMoves(fx)) return true;
        for (const auto &c : n.children) if (moves(c)) return true;
        return false;
    };
    for (const auto &n : d.nodes) if (moves(n)) return true;
    return false;
}

std::vector<float> Components(const std::wstring &s) {
    if (s == L"true") return {1};
    if (s == L"false") return {0};
    if (!s.empty() && s[0] == L'#') {
        if (!Color(s)) return {};
        const uint32_t rgb = wcstoul(s.substr(1, 6).c_str(), nullptr, 16);
        const float a = s.size() == 9 ? wcstoul(s.substr(7, 2).c_str(), nullptr, 16) / 255.f : 1.f;
        return {(rgb >> 16) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    }
    std::vector<float> out;
    for (const auto &w : Words(s)) {
        double n = 0;
        if (out.size() == 4 || !Number(w, &n)) return {};
        out.push_back((float)n);
    }
    return out;
}

std::vector<std::wstring> EffectTypes() {
    std::vector<std::wstring> out;
    for (const auto &s : Effects()) out.push_back(s.type);
    return out;
}

namespace {
std::wstring Short(double v) { return Format(L"%g", v); }
std::wstring WordList(const wchar_t *const *words) {
    std::wstring s;
    for (; words && *words; ++words) s += (s.empty() ? L"`" : L" `") + std::wstring(*words) + L"`";
    return s;
}
std::wstring ValuesOf(const Prop &p, const std::wstring &element) {
    switch (p.type) {
    case Type::Number: return p.lo <= -kBig && p.hi >= kBig ? L"number" : L"number, " + Short(p.lo) + L" to " + Short(p.hi);
    case Type::Integer: return L"whole number, " + Short(p.lo) + L" to " + Short(p.hi);
    case Type::Bool: return L"`true` `false`";
    case Type::Enum: return WordList(p.words);
    case Type::Color: return L"colour, `#RRGGBB` or `#RRGGBBAA`";
    case Type::Colors: return L"colour stops, such as `#000000 0%, #FFFFFF 100%`";
    case Type::Pair: return L"two numbers, " + Short(p.lo) + L" to " + Short(p.hi);
    case Type::Range: return L"seconds, `min-max` or one number";
    case Type::Vector: return L"one to four numbers, or a colour";
    case Type::Asset:
        if (std::wstring(p.name) == L"mask") return L"a `<mask>` asset's id";
        return element == L"particles" ? L"an `<image>` or `<frames>` asset's id"
                                       : L"an `<image>`, `<frames>`, `<video>`, `<lottie>` or `kind=\"layer\"` `<shader>` asset's id";
    }
    return L"";
}
}  // namespace

std::vector<AttributeInfo> AttributeTable() {
    std::vector<AttributeInfo> out;
    auto add = [&](const std::wstring &element, const Prop *begin, const Prop *end) {
        for (const Prop *p = begin; p != end; ++p) out.push_back({element, p->name, ValuesOf(*p, element), p->fallback, p->animated});
    };
    add(L"node", std::begin(kCommon), std::end(kCommon));
    add(L"layer", std::begin(kLayer), std::end(kLayer));
    add(L"fill", std::begin(kFill), std::end(kFill));
    add(L"particles", std::begin(kParticles), std::end(kParticles));
    for (const auto &s : Effects()) add(L"effect " + std::wstring(s.type), s.props.data(), s.props.data() + s.props.size());
    return out;
}

std::vector<DataInfo> DataNames() {
    std::vector<DataInfo> out;
    for (const auto &d : kData) {
        DataInfo info{d.name, {}};
        for (const wchar_t *const *w = d.words; w && *w; ++w) info.words.push_back(*w);
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<std::array<std::wstring, 3>> PresetValues() {
    std::vector<std::array<std::wstring, 3>> out;
    for (const auto &v : kPresetValues) out.push_back({v.preset, v.name, v.value});
    return out;
}

// --- Files ---------------------------------------------------------------------------------

namespace {
constexpr uint32_t kEnvelopeMagic = 0x32454353;  // "SCE2"
void Put(std::vector<uint8_t> &out, uint32_t n) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(n >> (8 * i)));
}
uint32_t U32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// What an envelope may carry: the document, the still picture, a video's first frame as the
// importer hands it over, and files named as a package or the library names them.
bool EnvelopeName(std::wstring_view name) {
    return name == L"scene.xml" || name == L"still.bmp" || name == L"poster.bmp" || PackagePath(name);
}
}  // namespace

const File *Find(const Files &files, std::wstring_view name) {
    for (const auto &f : files) if (f.name == name) return &f;
    return nullptr;
}

std::vector<uint8_t> Pack(const Files &files) {
    if (files.size() > kMaxFiles) return {};
    std::vector<uint8_t> out;
    size_t total = 8;
    std::set<std::wstring> names;
    for (const auto &f : files) {
        if (!EnvelopeName(f.name) || !names.insert(f.name).second) return {};
        total += 8 + ToUtf8(f.name).size() + f.bytes.size();
        if (f.bytes.size() > kMaxBundleBytes || total > kMaxBundleBytes) return {};
    }
    out.reserve(total);
    Put(out, kEnvelopeMagic);
    Put(out, (uint32_t)files.size());
    for (const auto &f : files) {
        const std::string name = ToUtf8(f.name);
        Put(out, (uint32_t)name.size());
        Put(out, (uint32_t)f.bytes.size());
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), f.bytes.begin(), f.bytes.end());
    }
    return out;
}

bool Unpack(const uint8_t *bytes, size_t size, Files *files, std::wstring *why) {
    auto fail = [&] { if (why) *why = L"invalid scene envelope"; return false; };
    if (!bytes || size < 8 || size > kMaxBundleBytes || U32(bytes) != kEnvelopeMagic || U32(bytes + 4) > kMaxFiles) return fail();
    Files out;
    std::set<std::wstring> names;
    size_t at = 8;
    const uint32_t count = U32(bytes + 4);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (size - at < 8) return fail();
        const size_t n = U32(bytes + at), m = U32(bytes + at + 4);
        at += 8;
        if (!n || n > 128 || n > size - at || m > size - at - n) return fail();
        const std::string_view name((const char *)bytes + at, n);
        const std::wstring w = FromUtf8(name);
        at += n;
        if (ToUtf8(w) != name || !EnvelopeName(w) || !names.insert(w).second) return fail();
        out.push_back({w, {bytes + at, bytes + at + m}});
        at += m;
    }
    if (at != size) return fail();
    *files = std::move(out);
    return true;
}

bool IsPackagePath(std::wstring_view path) { return PackagePath(path); }

int FormatOf(std::string_view utf8) {
    xml::Element root;
    std::wstring error;
    xml::Limits limits;
    limits.bytes = kMaxTextBytes;
    limits.depth = 10;
    limits.elements = 4096;
    if (!xml::Parse(utf8, &root, &error, limits) || root.name != L"scene") return 0;
    const auto *format = root.Find(L"format");
    if (!format) return 0;
    if (*format == L"1") return 1;
    if (*format == L"2") return 2;
    return 0;
}

std::wstring ChosenName(const std::wstring &settingId) { return L"chosen/" + settingId + L".bmp"; }
std::wstring CompiledName(const Asset &a) { return L"shaders/" + a.id + L".dxbc"; }
std::wstring PosterName(const Asset &video) { return L"textures/" + video.id + L".tex"; }

std::vector<std::wstring> LibraryFiles(const Document &d) {
    std::vector<std::wstring> out;
    for (const auto &a : d.assets) {
        switch (a.kind) {
        case AssetKind::Video:
            out.push_back(PosterName(a));
            out.push_back(StoredName(a, 0));
            break;
        case AssetKind::LayerShader: case AssetKind::EffectShader:
            out.push_back(StoredName(a, 0));
            out.push_back(CompiledName(a));
            break;
        default:
            for (size_t i = 0; i < a.files.size(); ++i) out.push_back(StoredName(a, i));
        }
    }
    return out;
}

}  // namespace animelogon::scenegraph
