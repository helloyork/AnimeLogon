#include "animelogon/skins.h"

#include <windows.h>

#include "animelogon/paths.h"
#include "animelogon/secure.h"

namespace animelogon {
namespace {

constexpr size_t kMaxSkinBytes = 64 * 1024;

}  // namespace

std::wstring SkinsDir() { return paths::DataDir() + L"\\skins"; }
std::wstring SkinDir(const std::wstring &id) { return SkinsDir() + L"\\" + id; }
std::wstring SkinFilePath(const std::wstring &id) { return SkinDir(id) + L"\\skin.xml"; }

skin::Skin LoadSkin(const std::wstring &id, std::wstring *why) {
    if (id == L"default") return skin::Default();
    std::wstring reason;
    std::vector<uint8_t> bytes;
    skin::Skin s;
    const std::wstring path = SkinFilePath(id);
    if (!skin::IsSkinId(id)) reason = L"not a skin id";
    else if (!secure::IsTrusted(path, &reason)) reason = L"skin.xml " + reason;
    else if (!secure::ReadFileBytes(path, &bytes, kMaxSkinBytes)) reason = L"skin.xml could not be read";
    else if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &s, &reason))
        reason = L"skin.xml " + reason;
    else return s;
    if (why) *why = reason;
    return skin::Default();
}

std::vector<SkinEntry> ListSkins() {
    std::vector<SkinEntry> out;
    const skin::Skin &d = skin::Default();
    out.push_back({L"default", d.name, d.author});
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((SkinsDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring id = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || id == L"default" || !skin::IsSkinId(id)) continue;
        std::wstring why;
        const skin::Skin s = LoadSkin(id, &why);
        if (why.empty()) out.push_back({id, s.name, s.author});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

}  // namespace animelogon
