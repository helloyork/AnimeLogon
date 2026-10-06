#include "animelogon/components.h"

#include <windows.h>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxComponentBytes = 64 * 1024;

bool IsHexId(const std::wstring &id) {
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool LoadStored(const std::wstring &id, skin::Skin *component, std::wstring *why) {
    const std::wstring path = ComponentFilePath(id);
    std::vector<uint8_t> bytes;
    std::wstring reason;
    if (!secure::IsTrusted(path, &reason)) return *why = L"component.xml " + reason, false;
    if (!secure::ReadFileBytes(path, &bytes, kMaxComponentBytes)) return *why = L"component.xml could not be read", false;
    skin::Skin parsed;
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &reason))
        return *why = L"component.xml " + reason, false;
    *component = std::move(parsed);
    return true;
}

}  // namespace

bool IsComponentId(const std::wstring &id) { return id == kClockComponent || IsHexId(id); }

std::wstring NewComponentId() { return RandomHex(8); }

std::wstring ComponentsDir() { return paths::DataDir() + L"\\components"; }
std::wstring ComponentDir(const std::wstring &id) { return ComponentsDir() + L"\\" + id; }
std::wstring ComponentFilePath(const std::wstring &id) { return ComponentDir(id) + L"\\component.xml"; }

bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) {
    if (id == kClockComponent) {
        *component = skin::Default();
        return true;
    }
    std::wstring reason;
    if (!IsHexId(id)) reason = L"not a component id";
    else if (!IsDirectory(ComponentDir(id))) reason = L"no such component";
    else if (LoadStored(id, component, &reason)) return true;
    if (why) *why = reason;
    return false;
}

std::vector<ComponentEntry> ListComponents() {
    std::vector<ComponentEntry> out;
    const skin::Skin &clock = skin::Default();
    out.push_back({kClockComponent, clock.name, clock.author, true});
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((ComponentsDir() + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring id = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !IsHexId(id))
            continue;
        skin::Skin s;
        std::wstring why;
        if (LoadStored(id, &s, &why)) out.push_back({id, s.name, s.author, false});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
}

}  // namespace animelogon
