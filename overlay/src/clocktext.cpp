#include "clocktext.h"

#include <sddl.h>
#include <wtsapi32.h>

#include <vector>

#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"

namespace clocktext {
namespace {

using animelogon::RegionalFormat;

std::wstring RememberedPath() { return animelogon::paths::DataDir() + L"\\clock-locale"; }

std::wstring ReadString(HKEY key, const wchar_t *name) {
    wchar_t buf[128] = L"";
    DWORD bytes = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buf), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ)
        return {};
    buf[bytes / sizeof(wchar_t)] = L'\0';
    return buf;
}

std::wstring LocaleInfo(const std::wstring &locale, LCTYPE type) {
    wchar_t buf[128] = L"";
    return GetLocaleInfoEx(locale.c_str(), type | LOCALE_NOUSEROVERRIDE, buf, ARRAYSIZE(buf)) ? buf : L"";
}

RegionalFormat Defaults(const std::wstring &locale) {
    return {locale, LocaleInfo(locale, LOCALE_SSHORTTIME), LocaleInfo(locale, LOCALE_SLONGDATE)};
}

bool Usable(const RegionalFormat &f) {
    return IsValidLocaleName(f.locale.c_str()) && !f.shortTime.empty() && !f.longDate.empty();
}

// HKU\<sid>\Control Panel\International of the person signed in at the console.
bool ConsoleUser(RegionalFormat *f) {
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE token = nullptr;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) return false;
    std::vector<BYTE> user(256);
    DWORD need = 0;
    bool ok = GetTokenInformation(token, TokenUser, user.data(), (DWORD)user.size(), &need) != FALSE;
    CloseHandle(token);
    if (!ok) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid)) return false;
    const std::wstring path = std::wstring(sid) + L"\\Control Panel\\International";
    LocalFree(sid);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    f->locale = ReadString(key, L"LocaleName");
    f->shortTime = ReadString(key, L"sShortTime");
    f->longDate = ReadString(key, L"sLongDate");
    RegCloseKey(key);
    if (IsValidLocaleName(f->locale.c_str())) {
        const RegionalFormat d = Defaults(f->locale);
        if (f->shortTime.empty()) f->shortTime = d.shortTime;
        if (f->longDate.empty()) f->longDate = d.longDate;
    }
    return Usable(*f);
}

bool Remembered(RegionalFormat *f) {
    std::wstring why;
    std::vector<uint8_t> bytes;
    if (!animelogon::secure::IsTrusted(RememberedPath(), &why) ||
        !animelogon::secure::ReadFileBytes(RememberedPath(), &bytes, 1024))
        return false;
    return animelogon::ParseRegionalFormat(std::string(bytes.begin(), bytes.end()), f) && Usable(*f);
}

void Remember(const RegionalFormat &f) {
    RegionalFormat old;
    if (Remembered(&old) && old.locale == f.locale && old.shortTime == f.shortTime && old.longDate == f.longDate)
        return;
    const std::string bytes = animelogon::SerializeRegionalFormat(f);
    animelogon::secure::WriteBytes(RememberedPath(), bytes.data(), bytes.size());
}

RegionalFormat SystemFormat() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    if (!GetUserDefaultLocaleName(name, ARRAYSIZE(name))) wcscpy_s(name, L"en-US");
    return Defaults(name);
}

// zh-CN and zh-TW are different languages here; en-US and en-GB are not.
bool SameLanguage(const std::wstring &a, const std::wstring &b) {
    const std::wstring la = a.substr(0, a.find(L'-')), lb = b.substr(0, b.find(L'-'));
    if (la != lb) return false;
    if (la != L"zh") return true;
    auto traditional = [](const std::wstring &l) {
        return l == L"zh-TW" || l == L"zh-HK" || l == L"zh-MO" || l.find(L"Hant") != std::wstring::npos;
    };
    return traditional(a) == traditional(b);
}

}  // namespace

Pictures Resolve(const animelogon::ClockSettings &settings) {
    RegionalFormat f;
    const wchar_t *source = L"the signed-in user's regional format";
    if (ConsoleUser(&f)) {
        Remember(f);
    } else if (Remembered(&f)) {
        source = L"the last signed-in user's regional format";
    } else {
        f = SystemFormat();
        source = L"the system's format";
    }
    // A chosen language brings its own standard format, unless it is the one already in use.
    if (!settings.language.empty() && !SameLanguage(settings.language, f.locale) &&
        IsValidLocaleName(settings.language.c_str())) {
        f = Defaults(settings.language);
        source = L"the chosen language";
    }
    Pictures p;
    p.locale = f.locale;
    p.time = settings.hour24 ? animelogon::To24HourPicture(f.shortTime) : f.shortTime;
    switch (settings.date) {
    case animelogon::DateStyle::None: break;
    case animelogon::DateStyle::Long: p.date = f.longDate; break;
    case animelogon::DateStyle::Weekday: p.date = animelogon::WeekdayDatePicture(f.locale, f.longDate); break;
    }
    ALOG(L"clock: %s, %s", p.locale.c_str(), source);
    return p;
}

std::wstring Time(const Pictures &p, const SYSTEMTIME &t) {
    wchar_t buf[128] = L"";
    if (GetTimeFormatEx(p.locale.c_str(), 0, &t, p.time.c_str(), buf, ARRAYSIZE(buf))) return buf;
    return GetTimeFormatEx(p.locale.c_str(), TIME_NOSECONDS, &t, nullptr, buf, ARRAYSIZE(buf)) ? buf : L"";
}

std::wstring Date(const Pictures &p, const SYSTEMTIME &t) {
    if (p.date.empty()) return {};
    wchar_t buf[160] = L"";
    if (GetDateFormatEx(p.locale.c_str(), 0, &t, p.date.c_str(), buf, ARRAYSIZE(buf), nullptr)) return buf;
    return GetDateFormatEx(p.locale.c_str(), DATE_LONGDATE, &t, nullptr, buf, ARRAYSIZE(buf), nullptr) ? buf : L"";
}

}  // namespace clocktext
