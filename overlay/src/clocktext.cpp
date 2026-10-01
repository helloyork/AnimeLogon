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
        const RegionalFormat d = animelogon::StandardFormat(f->locale);
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
    return animelogon::StandardFormat(name);
}

}  // namespace

RegionalFormat UserFormat() {
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
    ALOG(L"clock: %s, %s", f.locale.c_str(), source);
    return f;
}

}  // namespace clocktext
