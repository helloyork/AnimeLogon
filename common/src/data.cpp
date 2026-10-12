#include "animelogon/data.h"

#include <algorithm>
#include <cmath>

namespace animelogon::data {
namespace {

// Segoe Fluent Icons (Windows 11) and Segoe MDL2 Assets (Windows 10) share these code points.
const wchar_t kBatteryGlyphs[] = {0xE850, 0xE851, 0xE852, 0xE853, 0xE854, 0xE855,
                                  0xE856, 0xE857, 0xE858, 0xE859, 0xE83F};
const wchar_t kChargingGlyphs[] = {0xE85A, 0xE85B, 0xE85C, 0xE85D, 0xE85E, 0xE85F,
                                   0xE860, 0xE861, 0xE862, 0xE83E, 0xEA93};
constexpr wchar_t kPlayGlyph = 0xE768, kPauseGlyph = 0xE769;

const std::vector<Variable> kVariables = {
    {L"time", Kind::Text, Source::Clock, {}, L"时间"},
    {L"ampm", Kind::Text, Source::Clock, {}, L"上午/下午"},
    {L"date", Kind::Text, Source::Clock, {}, L"日期"},
    {L"date.long", Kind::Text, Source::Clock, {}, L"日期"},
    {L"weekday", Kind::Text, Source::Clock, {}, L"星期"},
    {L"month", Kind::Text, Source::Clock, {}, L"月份"},
    {L"day", Kind::Text, Source::Clock, {}, L"日"},
    {L"year", Kind::Text, Source::Clock, {}, L"年份"},
    {L"battery.percent", Kind::Text, Source::Battery, {}, L"电量"},
    {L"battery.remaining", Kind::Text, Source::Battery, {}, L"剩余时间"},
    {L"battery.glyph", Kind::Text, Source::Battery, {}, L"电池图标"},
    {L"battery.level", Kind::Fraction, Source::Battery, {}, L"电量条"},
    {L"battery.state", Kind::State, Source::Battery, {L"none", L"charging", L"discharging", L"plugged"}, L""},
    {L"media.title", Kind::Text, Source::Media, {}, L"歌名"},
    {L"media.artist", Kind::Text, Source::Media, {}, L"歌手"},
    {L"media.album", Kind::Text, Source::Media, {}, L"专辑"},
    {L"media.app", Kind::Text, Source::Media, {}, L"播放应用"},
    {L"media.position", Kind::Text, Source::Media, {}, L"播放位置"},
    {L"media.duration", Kind::Text, Source::Media, {}, L"总时长"},
    {L"media.glyph", Kind::Text, Source::Media, {}, L"播放状态图标"},
    {L"media.progress", Kind::Fraction, Source::Media, {}, L"播放进度"},
    {L"media.state", Kind::State, Source::Media, {L"none", L"playing", L"paused"}, L""},
    {L"until.days", Kind::Text, Source::Until, {}, L"剩余天数"},
    {L"until.state", Kind::State, Source::Until, {L"none", L"ahead", L"today", L"past"}, L""},
};

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysIn(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && IsLeap(y) ? 29 : days[m - 1];
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
long long DayNumber(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (long long)doe - 719468;
}

// Days from today to the date: positive ahead, 0 on the day, negative after it.
long long DaysTo(const Date &until, const SYSTEMTIME &local) {
    return DayNumber(until.year, until.month, until.day) - DayNumber(local.wYear, local.wMonth, local.wDay);
}

// h:mm:ss under an hour as m:ss.
std::wstring Clock(double seconds, bool hoursAlways) {
    const long long s = (long long)std::floor(std::max(0.0, seconds));
    const long long h = s / 3600, m = s / 60 % 60, sec = s % 60;
    wchar_t buf[32];
    if (hoursAlways) swprintf_s(buf, L"%lld:%02lld", h, m);  // h:mm
    else if (h) swprintf_s(buf, L"%lld:%02lld:%02lld", h, m, sec);
    else swprintf_s(buf, L"%lld:%02lld", m, sec);
    return buf;
}

// Where playback has got to by `now`.
double PositionNow(const Media &m, uint64_t now) {
    double p = m.position;
    if (m.state == MediaState::Playing && m.at && now > m.at) p += (double)(now - m.at) / 1e7 * m.rate;
    if (m.duration > 0.0) p = std::min(p, m.duration);
    return std::max(0.0, p);
}

int Level(int percent) { return std::clamp((percent + 5) / 10, 0, 10); }

bool IsRemoved(wchar_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x200E || c == 0x200F ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x2028 || c == 0x2029 ||
           c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB);
}

}  // namespace

Battery ReadBattery() {
    Battery b;
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || s.BatteryFlag == 128 || s.BatteryFlag == 255) return b;
    if (s.BatteryLifePercent <= 100) b.percent = s.BatteryLifePercent;
    if (s.BatteryFlag & 8) b.state = BatteryState::Charging;
    else if (s.ACLineStatus == 1) b.state = BatteryState::Plugged;
    else b.state = BatteryState::Discharging;
    if (b.state == BatteryState::Discharging && s.BatteryLifeTime != (DWORD)-1) b.secondsLeft = (int)s.BatteryLifeTime;
    return b;
}

bool ParseDate(const std::wstring &text, Date *out) {
    if (text.size() != 10 || text[4] != L'-' || text[7] != L'-') return false;
    int parts[3] = {0, 0, 0};
    const size_t starts[] = {0, 5, 8}, lengths[] = {4, 2, 2};
    for (int k = 0; k < 3; ++k)
        for (size_t i = starts[k]; i < starts[k] + lengths[k]; ++i) {
            if (text[i] < L'0' || text[i] > L'9') return false;
            parts[k] = parts[k] * 10 + (text[i] - L'0');
        }
    if (parts[0] < 1601 || parts[1] < 1 || parts[1] > 12 || parts[2] < 1 || parts[2] > DaysIn(parts[0], parts[1]))
        return false;
    *out = {parts[0], parts[1], parts[2]};
    return true;
}

const Variable *Find(const std::wstring &name) {
    for (const Variable &v : kVariables)
        if (name == v.name) return &v;
    return nullptr;
}

const std::vector<Variable> &Variables() { return kVariables; }

const wchar_t *LabelOf(Source source) {
    switch (source) {
    case Source::Clock: return L"时间日期";
    case Source::Battery: return L"电量";
    case Source::Media: return L"正在播放";
    case Source::Until: return L"倒计时";
    }
    return L"";
}

bool ParseCondition(const std::wstring &text, Condition *out) {
    Condition c;
    std::wstring name = text;
    const size_t eq = text.find(L'=');
    if (eq != std::wstring::npos) {
        const bool negated = eq > 0 && text[eq - 1] == L'!';
        name = text.substr(0, negated ? eq - 1 : eq);
        c.op = negated ? Condition::Op::IsNot : Condition::Op::Is;
        c.value = text.substr(eq + 1);
    } else if (!text.empty() && text[0] == L'!') {
        name = text.substr(1);
        c.op = Condition::Op::HasNot;
    }
    const Variable *v = Find(name);
    if (!v) return false;
    if (c.op == Condition::Op::Is || c.op == Condition::Op::IsNot) {
        if (v->kind != Kind::State) return false;
        if (std::find(v->states.begin(), v->states.end(), c.value) == v->states.end()) return false;
    }
    c.name = name;
    *out = std::move(c);
    return true;
}

bool Fill(const std::wstring &name, const Context &context, const SYSTEMTIME &local, std::wstring *out) {
    const Variable *v = Find(name);
    if (!v || v->kind != Kind::Text || v->source == Source::Clock) return false;
    out->clear();
    const Snapshot empty;
    const Snapshot &s = context.snapshot ? *context.snapshot : empty;
    const Battery &b = s.battery;
    const Media &m = s.media;
    if (name == L"battery.percent") {
        if (b.state != BatteryState::None && b.percent >= 0) *out = std::to_wstring(b.percent);
    } else if (name == L"battery.remaining") {
        if (b.state == BatteryState::Discharging && b.secondsLeft > 0) *out = Clock(b.secondsLeft, true);
    } else if (name == L"battery.glyph") {
        if (b.state != BatteryState::None && b.percent >= 0) {
            const int level = Level(b.percent);
            *out = std::wstring(1, b.state == BatteryState::Discharging ? kBatteryGlyphs[level] : kChargingGlyphs[level]);
        }
    } else if (m.state == MediaState::None && name.compare(0, 6, L"media.") == 0) {
        // nothing is playing
    } else if (name == L"media.title") *out = m.title;
    else if (name == L"media.artist") *out = m.artist;
    else if (name == L"media.album") *out = m.album;
    else if (name == L"media.app") *out = m.app;
    else if (name == L"media.position") *out = Clock(PositionNow(m, context.now), false);
    else if (name == L"media.duration") {
        if (m.duration > 0.0) *out = Clock(m.duration, false);
    } else if (name == L"media.glyph") *out = std::wstring(1, m.state == MediaState::Playing ? kPlayGlyph : kPauseGlyph);
    else if (name == L"until.days") {
        if (context.until.valid() && DaysTo(context.until, local) > 0) *out = std::to_wstring(DaysTo(context.until, local));
    }
    return true;
}

bool FractionOf(const std::wstring &name, const Context &context, double *out) {
    const Snapshot empty;
    const Snapshot &s = context.snapshot ? *context.snapshot : empty;
    if (name == L"battery.level") {
        if (s.battery.state == BatteryState::None || s.battery.percent < 0) return false;
        *out = s.battery.percent / 100.0;
        return true;
    }
    if (name == L"media.progress") {
        if (s.media.state == MediaState::None || s.media.duration <= 0.0) return false;
        *out = std::clamp(PositionNow(s.media, context.now) / s.media.duration, 0.0, 1.0);
        return true;
    }
    return false;
}

std::wstring StateOf(const std::wstring &name, const Context &context, const SYSTEMTIME &local) {
    const Snapshot empty;
    const Snapshot &s = context.snapshot ? *context.snapshot : empty;
    if (name == L"battery.state") {
        switch (s.battery.state) {
        case BatteryState::Charging: return L"charging";
        case BatteryState::Discharging: return L"discharging";
        case BatteryState::Plugged: return L"plugged";
        case BatteryState::None: break;
        }
        return L"none";
    }
    if (name == L"media.state")
        return s.media.state == MediaState::Playing ? L"playing" : s.media.state == MediaState::Paused ? L"paused" : L"none";
    if (name == L"until.state") {
        if (!context.until.valid()) return L"none";
        const long long days = DaysTo(context.until, local);
        return days > 0 ? L"ahead" : days == 0 ? L"today" : L"past";
    }
    return L"";
}

std::wstring CleanText(const std::wstring &text, size_t max) {
    std::wstring out;
    out.reserve(std::min(text.size(), max));
    bool space = false;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        // A surrogate is kept only as half of a pair.
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                if (space && !out.empty()) out += L' ';
                space = false;
                out += c;
                out += text[++i];
            }
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) continue;
        if (c == L'\t' || c == L'\n' || c == L'\r' || c == 0x2028 || c == 0x2029 || c == L' ' || c == 0x3000) {
            space = true;
            continue;
        }
        if (IsRemoved(c)) continue;
        if (space && !out.empty()) out += L' ';
        space = false;
        out += c;
    }
    if (out.size() > max) {
        size_t cut = max;
        if (cut > 0 && out[cut - 1] >= 0xD800 && out[cut - 1] <= 0xDBFF) --cut;
        out.resize(cut);
        while (!out.empty() && out.back() == L' ') out.pop_back();
    }
    return out;
}

}  // namespace animelogon::data
