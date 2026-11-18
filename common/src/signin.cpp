#include "animelogon/signin.h"

#include <windows.h>

#include <set>
#include <vector>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/sha256.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"

namespace animelogon::signin {
namespace {

constexpr size_t kMaxLine = 2048;
constexpr size_t kNameDigits = 16;

// 16 lowercase hex digits and ".png".
bool IsImageName(std::wstring_view name) {
    if (name.size() != kNameDigits + 4 || name.substr(kNameDigits) != L".png") return false;
    for (size_t i = 0; i < kNameDigits; ++i) {
        const wchar_t c = name[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

}  // namespace

std::wstring Path() { return paths::DataDir() + L"\\signin.ini"; }
std::wstring Dir() { return paths::DataDir() + L"\\signin"; }

std::wstring ImageFor(const std::wstring &key) {
    const std::string bytes = ToUtf8(key);
    const std::wstring hash = Sha256Of(bytes.data(), bytes.size());
    return Dir() + L"\\" + hash.substr(0, kNameDigits) + L".png";
}

std::string Serialize(const Want &want) {
    const size_t slash = want.image.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? want.image : want.image.substr(slash + 1);
    return "format=1\nkey=" + ToUtf8(want.key) + "\ntheme=" + ToUtf8(want.theme) + "\nimage=" + ToUtf8(name) + "\n";
}

bool Parse(std::string_view text, Want *out, std::wstring *why) {
    auto fail = [&](const wchar_t *what) {
        if (why) *why = what;
        return false;
    };
    if (text.size() > kMaxBytes) return fail(L"too long");
    Want want;
    std::set<std::string> seen;
    bool format = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.size() > kMaxLine) return fail(L"a line is too long");
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return fail(L"a line is not key=value");
        const std::string key(line.substr(0, eq));
        const std::wstring value = FromUtf8(line.substr(eq + 1));
        if (!seen.insert(key).second) return fail(L"a key is given twice");
        if (key == "format") {
            if (value != L"1") return fail(L"an unknown format");
            format = true;
        } else if (key == "key") {
            if (value.empty() || value.size() > 1024) return fail(L"a key out of shape");
            want.key = value;
        } else if (key == "theme") {
            if (!IsThemeId(value)) return fail(L"a theme id out of shape");
            want.theme = value;
        } else if (key == "image") {
            if (!IsImageName(value)) return fail(L"an image name out of shape");
            want.image = Dir() + L"\\" + value;
        } else {
            return fail(L"an unknown key");
        }
    }
    if (!format || want.key.empty() || want.image.empty()) return fail(L"a key is missing");
    *out = std::move(want);
    return true;
}

bool Load(Want *out, std::wstring *why) {
    const std::wstring path = Path();
    std::wstring reason;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (why) *why = L"no signin.ini";
        return false;
    }
    if (!secure::IsTrusted(path, &reason)) {
        if (why) *why = L"signin.ini " + reason;
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(path, &bytes, kMaxBytes)) {
        if (why) *why = L"signin.ini could not be read";
        return false;
    }
    return Parse(std::string_view((const char *)bytes.data(), bytes.size()), out, why);
}

}  // namespace animelogon::signin
