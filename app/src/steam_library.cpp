#include "steam_library.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "animelogon/secure.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace app::steam_library {

namespace {

constexpr size_t kMaxVdfBytes = 1024 * 1024;

std::wstring Lower(std::wstring text) {
    for (wchar_t &c : text) c = (wchar_t)std::towlower(c);
    return text;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring RegistryText(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t text[MAX_PATH * 2];
    DWORD size = sizeof(text);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS) return L"";
    return text;
}

// Steam's text files (.vdf, .acf) are quoted strings (with \\ and \" escapes), braces and //
// comments. A string followed by a string is a key and its value; a string followed by a brace
// opens a block.
struct Token {
    bool text;
    std::string value;  // the text, or "{" / "}"
};

std::vector<Token> Tokens(std::string_view vdf) {
    std::vector<Token> tokens;
    for (size_t i = 0; i < vdf.size();) {
        const char c = vdf[i];
        if (c == '"') {
            std::string value;
            for (++i; i < vdf.size() && vdf[i] != '"'; ++i) {
                if (vdf[i] == '\\' && i + 1 < vdf.size()) ++i;
                value += vdf[i];
            }
            ++i;
            tokens.push_back({true, value});
        } else if (c == '{' || c == '}') {
            tokens.push_back({false, std::string(1, c)});
            ++i;
        } else if (c == '/' && i + 1 < vdf.size() && vdf[i + 1] == '/') {
            while (i < vdf.size() && vdf[i] != '\n') ++i;
        } else {
            ++i;
        }
    }
    return tokens;
}

// Calls `pair(depth, key, value)` for every key with a string value, at the depth of braces it is in.
template <class F>
void EachPair(const std::vector<Token> &tokens, F pair) {
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].text) {
            depth += tokens[i].value == "{" ? 1 : -1;
            continue;
        }
        if (i + 1 >= tokens.size() || !tokens[i + 1].text) continue;  // a block's name
        pair(depth, tokens[i].value, tokens[i + 1].value);
        ++i;  // the value
    }
}

// A folder name with no way out of the folder it is in: no separators, no drive, not . or ..
bool PlainName(const std::wstring &name) {
    if (name.empty() || name == L"." || name == L".." || name.size() > 255) return false;
    for (wchar_t c : name)
        if (c < 0x20 || c == L'\\' || c == L'/' || c == L':') return false;
    return true;
}

}  // namespace

std::wstring Canonical(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring SteamDir() {
    std::wstring steam = RegistryText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    return steam.empty() ? steam : Canonical(steam);
}

std::vector<std::wstring> LibraryFolders(std::string_view vdf) {
    std::vector<std::wstring> folders;
    EachPair(Tokens(vdf), [&](int depth, const std::string &key, const std::string &value) {
        const bool numbered = !key.empty() && std::all_of(key.begin(), key.end(), [](char k) { return k >= '0' && k <= '9'; });
        if (_stricmp(key.c_str(), "path") == 0 || (depth == 1 && numbered)) {
            const std::wstring path = Canonical(FromUtf8(value));
            if (!path.empty()) folders.push_back(path);
        }
    });
    return folders;
}

std::wstring ManifestInstallDir(std::string_view acf) {
    // "AppState" { ... "installdir" "Wallpaper Engine" ... }: the key is in the outermost block.
    std::wstring dir;
    bool found = false;
    EachPair(Tokens(acf), [&](int depth, const std::string &key, const std::string &value) {
        if (found || depth != 1 || _stricmp(key.c_str(), "installdir") != 0) return;
        found = true;
        dir = FromUtf8(value);
    });
    return PlainName(dir) ? dir : L"";
}

std::vector<std::wstring> Libraries() {
    const std::wstring steam = SteamDir();
    if (steam.empty()) return {};
    // Steam itself is the first library; the others are listed in its libraryfolders.vdf (in
    // steamapps\ since 2021, in config\ as well).
    std::vector<std::wstring> libraries{steam};
    for (const wchar_t *list : {L"\\steamapps\\libraryfolders.vdf", L"\\config\\libraryfolders.vdf"}) {
        std::vector<uint8_t> bytes;
        if (!secure::ReadFileBytes(steam + list, &bytes, kMaxVdfBytes)) continue;
        for (const std::wstring &l : LibraryFolders(std::string_view((const char *)bytes.data(), bytes.size())))
            libraries.push_back(l);
    }
    std::vector<std::wstring> unique, seen;
    for (const std::wstring &l : libraries) {
        const std::wstring key = Lower(l);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        unique.push_back(l);
    }
    return unique;
}

std::wstring WorkshopContentPath(const std::wstring &library, uint32_t appid) {
    return Canonical(library) + L"\\steamapps\\workshop\\content\\" + std::to_wstring(appid);
}

std::vector<std::wstring> WorkshopContentIn(const std::vector<std::wstring> &libraries, uint32_t appid) {
    std::vector<std::wstring> folders, seen;
    for (const std::wstring &library : libraries) {
        const std::wstring path = WorkshopContentPath(library, appid);
        const std::wstring key = Lower(path);
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || !IsDirectory(path)) continue;
        seen.push_back(key);
        folders.push_back(path);
    }
    return folders;
}

std::wstring AppInstallDirIn(const std::vector<std::wstring> &libraries, uint32_t appid) {
    for (const std::wstring &library : libraries) {
        const std::wstring apps = Canonical(library) + L"\\steamapps";
        std::vector<uint8_t> bytes;
        if (!secure::ReadFileBytes(apps + L"\\appmanifest_" + std::to_wstring(appid) + L".acf", &bytes, kMaxVdfBytes))
            continue;
        const std::wstring dir = ManifestInstallDir(std::string_view((const char *)bytes.data(), bytes.size()));
        if (dir.empty()) continue;
        const std::wstring path = apps + L"\\common\\" + dir;
        if (IsDirectory(path)) return path;
    }
    return L"";
}

std::vector<std::wstring> WorkshopContent(uint32_t appid) { return WorkshopContentIn(Libraries(), appid); }

std::wstring AppInstallDir(uint32_t appid) { return AppInstallDirIn(Libraries(), appid); }

}  // namespace app::steam_library
