#include "animelogon/clock.h"

#include <cwchar>

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

const wchar_t *const kAnchors[] = {L"top-left", L"top", L"top-right", L"left", L"center",
                                   L"right", L"bottom-left", L"bottom", L"bottom-right"};
const wchar_t *const kSizes[] = {L"small", L"medium", L"large", L"huge"};
const wchar_t *const kDisplays[] = {L"auto", L"primary", L"all"};
const wchar_t *const kDates[] = {L"none", L"long", L"weekday"};

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

std::wstring To24HourPicture(const std::wstring &picture) {
    std::wstring out = picture;
    std::wstring marked(out.size(), L' ');
    ForEachUnquoted(out, [&](size_t i) {
        if (out[i] == L'h') out[i] = L'H';
        else if (out[i] == L't') marked[i] = L't';
    });
    // Drop the AM/PM marker and the spaces that set it apart.
    std::wstring result;
    for (size_t i = 0; i < out.size(); ++i)
        if (marked[i] != L't') result += out[i];
    size_t a = 0, b = result.size();
    while (a < b && result[a] == L' ') ++a;
    while (b > a && result[b - 1] == L' ') --b;
    result = result.substr(a, b - a);
    for (size_t i = result.find(L"  "); i != std::wstring::npos; i = result.find(L"  ")) result.erase(i, 1);
    return result.empty() ? L"HH:mm" : result;
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

const wchar_t *ToString(ClockAnchor anchor) { return kAnchors[(int)anchor]; }
const wchar_t *ToString(ClockSize size) { return kSizes[(int)size]; }
const wchar_t *ToString(ClockDisplays displays) { return kDisplays[(int)displays]; }
const wchar_t *ToString(DateStyle style) { return kDates[(int)style]; }
bool Parse(const std::wstring &text, ClockAnchor *out) { return ParseName(text, kAnchors, out); }
bool Parse(const std::wstring &text, ClockSize *out) { return ParseName(text, kSizes, out); }
bool Parse(const std::wstring &text, ClockDisplays *out) { return ParseName(text, kDisplays, out); }
bool Parse(const std::wstring &text, DateStyle *out) { return ParseName(text, kDates, out); }

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
