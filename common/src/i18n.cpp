#include "animelogon/i18n.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "animelogon/paths.h"
#include "animelogon/text.h"

namespace animelogon::i18n {

namespace {

struct Info {
    Lang lang;
    const wchar_t *tag, *name, *locale, *resource;
};
// The resource names match res/locales.rc.in.
constexpr Info kInfo[] = {
    {Lang::ZhHans, L"zh-Hans", L"简体中文", L"zh-CN", L"LOCALE_ZH_HANS"},
    {Lang::ZhHant, L"zh-Hant", L"繁體中文", L"zh-TW", L"LOCALE_ZH_HANT"},
    {Lang::En, L"en", L"English", L"en-US", L"LOCALE_EN"},
    {Lang::Ja, L"ja", L"日本語", L"ja-JP", L"LOCALE_JA"},
};
const Info &InfoOf(Lang lang) {
    for (const Info &i : kInfo)
        if (i.lang == lang) return i;
    return kInfo[0];
}

// The person's choice lives beside nothing else of theirs; the machine's beside InstallDir.
constexpr const wchar_t *kUserKey = L"Software\\AnimeLogon";
constexpr const wchar_t *kValue = L"Language";

std::wstring ReadString(HKEY root, const wchar_t *key) {
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegGetValueW(root, key, kValue, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

DWORD WriteString(HKEY root, const wchar_t *key, const std::wstring &tag) {
    HKEY h = nullptr;
    LSTATUS e = RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &h, nullptr);
    if (e != ERROR_SUCCESS) return (DWORD)e;
    if (tag.empty()) {
        e = RegDeleteValueW(h, kValue);
        if (e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS;
    } else {
        e = RegSetValueExW(h, kValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(tag.c_str()),
                           (DWORD)((tag.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(h);
    return (DWORD)e;
}

/* --- the files --- */

using Map = std::unordered_map<std::string, std::wstring>;

struct Tables {
    Lang lang = Lang::ZhHans;
    Map own, fallback;  // the language's strings, and Simplified Chinese's
};

std::shared_ptr<const Tables> g_tables;
std::mutex g_loading;

std::shared_ptr<const Tables> Get() {
    std::shared_ptr<const Tables> t = std::atomic_load(&g_tables);
    if (t) return t;
    Use(Preferred());
    return std::atomic_load(&g_tables);
}

// A flat JSON object of strings, as the files are written: anything else fails the whole file.
class Parser {
public:
    explicit Parser(std::wstring_view text) : s_(text) {}

    bool Run(void (*each)(void *, std::string_view, const std::wstring &), void *context) {
        Space();
        if (!Eat(L'{')) return false;
        Space();
        if (Eat(L'}')) return End();
        for (;;) {
            std::wstring key, value;
            Space();
            if (!String(&key)) return false;
            Space();
            if (!Eat(L':')) return false;
            Space();
            if (!String(&value)) return false;
            const std::string k = ToUtf8(key);
            each(context, k, value);
            Space();
            if (Eat(L',')) continue;
            if (Eat(L'}')) return End();
            return false;
        }
    }

private:
    bool End() {
        Space();
        return i_ == s_.size();
    }
    void Space() {
        while (i_ < s_.size() && (s_[i_] == L' ' || s_[i_] == L'\t' || s_[i_] == L'\r' || s_[i_] == L'\n')) ++i_;
    }
    bool Eat(wchar_t c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool Hex4(unsigned *out) {
        if (i_ + 4 > s_.size()) return false;
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const wchar_t c = s_[i_++];
            v <<= 4;
            if (c >= L'0' && c <= L'9') v |= c - L'0';
            else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
            else return false;
        }
        *out = v;
        return true;
    }
    bool String(std::wstring *out) {
        if (!Eat(L'"')) return false;
        while (i_ < s_.size()) {
            const wchar_t c = s_[i_++];
            if (c == L'"') return true;
            if (c < 0x20) return false;
            if (c != L'\\') {
                out->push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const wchar_t e = s_[i_++];
            switch (e) {
            case L'"': out->push_back(L'"'); break;
            case L'\\': out->push_back(L'\\'); break;
            case L'/': out->push_back(L'/'); break;
            case L'b': out->push_back(L'\b'); break;
            case L'f': out->push_back(L'\f'); break;
            case L'n': out->push_back(L'\n'); break;
            case L'r': out->push_back(L'\r'); break;
            case L't': out->push_back(L'\t'); break;
            case L'u': {
                unsigned v = 0;
                if (!Hex4(&v)) return false;
                out->push_back((wchar_t)v);  // UTF-16 code units: surrogate pairs arrive as two
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    std::wstring_view s_;
    size_t i_ = 0;
};

void Collect(void *context, std::string_view key, const std::wstring &value) {
    (*static_cast<Map *>(context))[std::string(key)] = value;
}

Map ParseOrEmpty(std::string_view json) {
    Map map;
    if (!ParseTable(json, Collect, &map)) map.clear();
    return map;
}

std::string_view Resource(Lang lang) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, InfoOf(lang).resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return {};
    HGLOBAL g = LoadResource(self, r);
    const void *data = g ? LockResource(g) : nullptr;
    return data ? std::string_view(static_cast<const char *>(data), SizeofResource(self, r)) : std::string_view();
}

const std::wstring *Find(const Tables &t, std::string_view key) {
    const std::string k(key);
    auto i = t.own.find(k);
    if (i != t.own.end()) return &i->second;
    i = t.fallback.find(k);
    return i != t.fallback.end() ? &i->second : nullptr;
}

}  // namespace

const wchar_t *Tag(Lang lang) { return InfoOf(lang).tag; }
const wchar_t *NativeName(Lang lang) { return InfoOf(lang).name; }
const wchar_t *Locale(Lang lang) { return InfoOf(lang).locale; }

bool FromTag(std::wstring_view tag, Lang *lang) {
    for (const Info &i : kInfo)
        if (EqualsNoCase(tag, i.tag)) {
            *lang = i.lang;
            return true;
        }
    return false;
}

bool FromLocaleName(std::wstring_view name, Lang *lang) {
    std::wstring n;
    for (wchar_t c : name) n.push_back(c == L'_' ? L'-' : (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c);
    auto starts = [&](std::wstring_view p) { return n.size() >= p.size() && n.compare(0, p.size(), p) == 0 && (n.size() == p.size() || n[p.size()] == L'-'); };
    if (starts(L"ja")) return *lang = Lang::Ja, true;
    if (starts(L"en")) return *lang = Lang::En, true;
    if (starts(L"zh")) {
        // Script first, then the regions that write Traditional characters.
        const bool hant = n.find(L"-hant") != std::wstring::npos ||
                          (n.find(L"-hans") == std::wstring::npos &&
                           (n.find(L"-tw") != std::wstring::npos || n.find(L"-hk") != std::wstring::npos ||
                            n.find(L"-mo") != std::wstring::npos));
        *lang = hant ? Lang::ZhHant : Lang::ZhHans;
        return true;
    }
    return false;
}

Lang SystemLanguage() {
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
        std::vector<wchar_t> buf(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) {
            for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) {
                Lang lang;
                if (FromLocaleName(p, &lang)) return lang;
            }
        }
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    Lang lang;
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) &&
        FromLocaleName(name, &lang))
        return lang;
    return Lang::En;
}

// A sandbox window keeps to its folder (paths::UseSandbox): its choice is a file there, so that
// trying a language in one leaves the installed app's alone.
std::wstring SandboxChoicePath() { return paths::UserDataDir() + L"\\language.txt"; }

std::wstring UserChoice() {
    if (paths::Sandbox().empty()) return ReadString(HKEY_CURRENT_USER, kUserKey);
    HANDLE h = CreateFileW(SandboxChoicePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[32] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    return std::wstring(Trim(FromUtf8(std::string_view(buf, got))));
}

DWORD SetUserChoice(const std::wstring &tag) {
    if (paths::Sandbox().empty()) return WriteString(HKEY_CURRENT_USER, kUserKey, tag);
    const std::wstring path = SandboxChoicePath();
    if (tag.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : GetLastError();
    paths::CreateDirectories(paths::UserDataDir());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    const std::string text = ToUtf8(tag);
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return e;
}
std::wstring MachineChoice() { return ReadString(HKEY_LOCAL_MACHINE, paths::kRegistryKey); }
DWORD SetMachineChoice(const std::wstring &tag) { return WriteString(HKEY_LOCAL_MACHINE, paths::kRegistryKey, tag); }

Lang Preferred() {
    Lang lang;
    if (FromTag(UserChoice(), &lang)) return lang;
    if (FromTag(MachineChoice(), &lang)) return lang;
    return SystemLanguage();
}

void Use(Lang lang) {
    UseText(lang, Resource(lang), Resource(Lang::ZhHans));
}

void UseText(Lang lang, std::string_view json, std::string_view fallbackJson) {
    std::lock_guard<std::mutex> hold(g_loading);
    auto t = std::make_shared<Tables>();
    t->lang = lang;
    t->own = ParseOrEmpty(json);
    if (lang != Lang::ZhHans) t->fallback = ParseOrEmpty(fallbackJson);
    std::atomic_store(&g_tables, std::shared_ptr<const Tables>(std::move(t)));
}

Lang Current() { return Get()->lang; }

std::wstring Tr(std::string_view key) {
    const auto t = Get();
    const std::wstring *s = Find(*t, key);
    return s ? *s : FromUtf8(key);
}

std::wstring Tr(std::string_view key, Args args) {
    const std::wstring text = Tr(key);
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'{') {
            const size_t close = text.find(L'}', i + 1);
            if (close != std::wstring::npos) {
                const std::wstring_view name(text.data() + i + 1, close - i - 1);
                bool replaced = false;
                for (const auto &a : args)
                    if (name == a.first) {
                        out += a.second;
                        replaced = true;
                        break;
                    }
                if (replaced) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

bool Has(std::string_view key) { return Find(*Get(), key) != nullptr; }

std::wstring Label(const std::wstring &text) {
    if (text.empty()) return text;
    const auto t = Get();
    if (const std::wstring *s = Find(*t, "label." + ToUtf8(text))) return *s;
    // "面板 2": the words, a space and a number.
    const size_t space = text.find_last_of(L' ');
    if (space != std::wstring::npos && space + 1 < text.size() &&
        text.find_first_not_of(L"0123456789", space + 1) == std::wstring::npos) {
        if (const std::wstring *s = Find(*t, "label." + ToUtf8(text.substr(0, space)) + " {n}")) {
            std::wstring out = *s;
            const size_t at = out.find(L"{n}");
            if (at != std::wstring::npos) out.replace(at, 3, text.substr(space + 1));
            return out;
        }
    }
    return text;
}

bool ParseTable(std::string_view json, void (*each)(void *, std::string_view, const std::wstring &), void *context) {
    if (json.size() >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
        json.remove_prefix(3);
    if (json.empty()) return false;
    const std::wstring text = FromUtf8(json);
    return Parser(text).Run(each, context);
}

}  // namespace animelogon::i18n
