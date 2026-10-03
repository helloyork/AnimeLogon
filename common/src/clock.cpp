#include "animelogon/clock.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxPicture = 80;

template <typename E, size_t N>
bool ParseName(const std::wstring &text, const wchar_t *const (&names)[N], E *out) {
    for (size_t i = 0; i < N; ++i)
        if (EqualsNoCase(text, names[i])) return *out = (E)i, true;
    return false;
}

const wchar_t *const kHours[] = {L"auto", L"12", L"24"};
const wchar_t *const kDates[] = {L"weekday", L"long", L"none"};

// Calls `visit(i)` for each character of `picture` outside a quoted literal.
template <typename F>
void ForEachUnquoted(const std::wstring &picture, F visit) {
    bool quoted = false;
    for (size_t i = 0; i < picture.size(); ++i) {
        if (picture[i] == L'\'') {
            quoted = !quoted;
            continue;
        }
        if (!quoted) visit(i);
    }
}

bool IsPictureText(const std::wstring &s) {
    if (s.empty() || s.size() > kMaxPicture) return false;
    for (wchar_t c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

std::wstring Language(const std::wstring &locale) { return locale.substr(0, locale.find(L'-')); }

// zh-CN and zh-TW are different languages here; en-US and en-GB are not.
bool SameLanguage(const std::wstring &a, const std::wstring &b) {
    const std::wstring la = Language(a), lb = Language(b);
    if (la != lb) return false;
    if (la != L"zh") return true;
    auto traditional = [](const std::wstring &l) {
        return l == L"zh-TW" || l == L"zh-HK" || l == L"zh-MO" || l.find(L"Hant") != std::wstring::npos;
    };
    return traditional(a) == traditional(b);
}

// Drops the AM/PM marker and the spaces that set it apart.
std::wstring DropMarker(const std::wstring &picture) {
    std::wstring marked(picture.size(), L' ');
    ForEachUnquoted(picture, [&](size_t i) {
        if (picture[i] == L't') marked[i] = L't';
    });
    std::wstring result;
    for (size_t i = 0; i < picture.size(); ++i)
        if (marked[i] != L't') result += picture[i];
    size_t a = 0, b = result.size();
    while (a < b && result[a] == L' ') ++a;
    while (b > a && result[b - 1] == L' ') --b;
    result = result.substr(a, b - a);
    for (size_t i = result.find(L"  "); i != std::wstring::npos; i = result.find(L"  ")) result.erase(i, 1);
    return result;
}

std::wstring Formatted(const std::wstring &locale, const SYSTEMTIME &t, const std::wstring &picture, bool date) {
    wchar_t buf[160] = L"";
    const int n = date ? GetDateFormatEx(locale.c_str(), 0, &t, picture.c_str(), buf, ARRAYSIZE(buf), nullptr)
                       : GetTimeFormatEx(locale.c_str(), 0, &t, picture.c_str(), buf, ARRAYSIZE(buf));
    return n > 0 ? buf : L"";
}

}  // namespace

bool IsClockLanguage(const std::wstring &locale) {
    static const wchar_t *const known[] = {L"zh-CN", L"zh-TW", L"ja-JP", L"ko-KR", L"en-US"};
    for (const wchar_t *k : known)
        if (locale == k) return true;
    return false;
}

bool IsFontFamilyName(const std::wstring &name) {
    if (name.empty() || name.size() > 64) return false;
    for (wchar_t c : name)
        if (c < 0x20 || c == 0x7F || c == L'=' || c == L'\\' || c == L'/') return false;
    return Trim(name).size() == name.size();
}

bool HasMarker(const std::wstring &picture) {
    bool marked = false;
    ForEachUnquoted(picture, [&](size_t i) { marked = marked || picture[i] == L't'; });
    return marked;
}

std::wstring WithoutMarkerPicture(const std::wstring &picture) {
    const std::wstring result = DropMarker(picture);
    return result.empty() ? L"h:mm" : result;
}

std::wstring To24HourPicture(const std::wstring &picture) {
    std::wstring out = picture;
    ForEachUnquoted(out, [&](size_t i) {
        if (out[i] == L'h') out[i] = L'H';
    });
    const std::wstring result = DropMarker(out);
    return result.empty() ? L"HH:mm" : result;
}

std::wstring To12HourPicture(const std::wstring &picture) {
    std::wstring out;
    bool quoted = false;
    for (size_t i = 0; i < picture.size(); ++i) {
        const wchar_t c = picture[i];
        if (c == L'\'') quoted = !quoted;
        if (quoted || c != L'H') {
            out += c;
        } else {
            out += L'h';  // HH becomes h too: 12-hour times are not zero-padded
            if (i + 1 < picture.size() && picture[i + 1] == L'H') ++i;
        }
    }
    return out;
}

std::wstring WeekdayDatePicture(const std::wstring &locale, const std::wstring &longDate) {
    bool named = false;
    ForEachUnquoted(longDate, [&](size_t i) {
        if (longDate.compare(i, 4, L"dddd") == 0) named = true;
    });
    if (named) return longDate;
    const std::wstring lang = Language(locale);
    // Chinese, Japanese and Korean put the weekday after the date.
    if (lang == L"zh" || lang == L"ja" || lang == L"ko") return longDate + L" dddd";
    return L"dddd, " + longDate;
}

std::wstring WithoutYearPicture(const std::wstring &picture) {
    // Splits the picture into quoted literals, runs of one format letter, and everything else.
    enum Kind { Literal, Field, Separator };
    struct Piece {
        std::wstring text;
        Kind kind;
    };
    std::vector<Piece> pieces;
    for (size_t i = 0; i < picture.size();) {
        size_t j = i + 1;
        Kind kind = Separator;
        if (picture[i] == L'\'') {
            while (j < picture.size() && picture[j] != L'\'') ++j;
            j = std::min(j + 1, picture.size());
            kind = Literal;
        } else if (std::iswalpha(picture[i])) {
            while (j < picture.size() && picture[j] == picture[i]) ++j;
            kind = Field;
        } else {
            while (j < picture.size() && picture[j] != L'\'' && !std::iswalpha(picture[j])) ++j;
        }
        pieces.push_back({picture.substr(i, j - i), kind});
        i = j;
    }
    size_t year = pieces.size();
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].kind == Field && pieces[i].text[0] == L'y') year = i;
    if (year == pieces.size()) return picture;

    // The year's own word sits next to it: 2026年 and 2026년 after it, "de 2026" before it.
    size_t from = year, to = year + 1;
    if (to < pieces.size() && pieces[to].kind == Literal) ++to;
    const bool last = std::all_of(pieces.begin() + (ptrdiff_t)to, pieces.end(),
                                  [](const Piece &p) { return p.kind != Field; });
    if (last) {
        to = pieces.size();  // trailing words such as "г." go with it
        while (from > 0 && pieces[from - 1].kind != Field) --from;
    } else {
        while (to < pieces.size() && pieces[to].kind == Separator) ++to;
    }
    std::wstring out;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (i < from || i >= to) out += pieces[i].text;
    return std::wstring(Trim(out));
}

std::string SerializeRegionalFormat(const RegionalFormat &f) {
    return ToUtf8(f.locale) + "\n" + ToUtf8(f.shortTime) + "\n" + ToUtf8(f.longDate) + "\n";
}

bool ParseRegionalFormat(const std::string &text, RegionalFormat *f) {
    const std::wstring all = FromUtf8(text);
    std::wstring lines[3];
    size_t at = 0;
    for (std::wstring &line : lines) {
        const size_t end = all.find(L'\n', at);
        if (end == std::wstring::npos) return false;
        line = std::wstring(Trim(std::wstring_view(all).substr(at, end - at)));
        at = end + 1;
    }
    if (lines[0].empty() || lines[0].size() > 84 || !IsPictureText(lines[1]) || !IsPictureText(lines[2]))
        return false;
    for (wchar_t c : lines[0])
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' ||
              c == L'_'))
            return false;
    f->locale = lines[0];
    f->shortTime = lines[1];
    f->longDate = lines[2];
    return true;
}

RegionalFormat StandardFormat(const std::wstring &locale) {
    auto info = [&](LCTYPE type) {
        wchar_t buf[128] = L"";
        return GetLocaleInfoEx(locale.c_str(), type | LOCALE_NOUSEROVERRIDE, buf, ARRAYSIZE(buf)) ? std::wstring(buf)
                                                                                               : std::wstring();
    };
    return {locale, info(LOCALE_SSHORTTIME), info(LOCALE_SLONGDATE)};
}

RegionalFormat FormatFor(const RegionalFormat &user, const std::wstring &locale) {
    if (locale.empty() || SameLanguage(locale, user.locale) || !IsValidLocaleName(locale.c_str())) return user;
    return StandardFormat(locale);
}

std::wstring FillText(const std::wstring &text, const RegionalFormat &format, const ClockStyle &style,
                      const SYSTEMTIME &t) {
    const std::wstring &time = format.shortTime;
    const ClockHours hours = style.hours;
    const bool marker = style.ampm && (hours == ClockHours::H12 || (hours == ClockHours::Auto && HasMarker(time)));
    const bool dated = style.date != ClockDate::None;
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        const size_t end = text[i] == L'{' ? text.find(L'}', i) : std::wstring::npos;
        if (end == std::wstring::npos) {
            out += text[i];
            continue;
        }
        const std::wstring name = text.substr(i + 1, end - i - 1);
        i = end;
        if (name == L"time")
            out += Formatted(format.locale, t,
                             hours == ClockHours::H24   ? To24HourPicture(time)
                             : hours == ClockHours::H12 ? WithoutMarkerPicture(To12HourPicture(time))
                                                        : WithoutMarkerPicture(time),
                             false);
        else if (name == L"ampm" && marker) out += Formatted(format.locale, t, L"tt", false);
        else if (!dated) continue;  // without a date, every part of one is empty
        else if (name == L"date")
            out += Formatted(format.locale, t,
                             style.date == ClockDate::Long
                                 ? format.longDate
                                 : WithoutYearPicture(WeekdayDatePicture(format.locale, format.longDate)),
                             true);
        else if (name == L"date.long") out += Formatted(format.locale, t, format.longDate, true);
        else if (name == L"weekday") out += Formatted(format.locale, t, L"dddd", true);
        else if (name == L"month") out += Formatted(format.locale, t, L"MMMM", true);
        else if (name == L"day") out += Formatted(format.locale, t, L"d", true);
        else if (name == L"year") out += Formatted(format.locale, t, L"yyyy", true);
    }
    return out;
}

const wchar_t *ToString(ClockHours hours) { return kHours[(int)hours]; }
bool Parse(const std::wstring &text, ClockHours *out) { return ParseName(text, kHours, out); }
const wchar_t *ToString(ClockDate date) { return kDates[(int)date]; }
bool Parse(const std::wstring &text, ClockDate *out) { return ParseName(text, kDates, out); }

bool ParseColor(const std::wstring &text, uint32_t *rgb) {
    if (text.size() != 7 || text[0] != L'#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const wchar_t c = text[i];
        const int d = c >= L'0' && c <= L'9'   ? c - L'0'
                      : c >= L'a' && c <= L'f' ? c - L'a' + 10
                      : c >= L'A' && c <= L'F' ? c - L'A' + 10
                                               : -1;
        if (d < 0) return false;
        v = v << 4 | (uint32_t)d;
    }
    *rgb = v;
    return true;
}

std::wstring FormatColor(uint32_t rgb) { return Format(L"#%06X", rgb & 0xFFFFFF); }

}  // namespace animelogon
