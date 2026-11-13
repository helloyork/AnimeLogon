#include "workshop.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "animelogon/secure.h"
#include "animelogon/text.h"

#include "json.h"

using namespace animelogon;

namespace app::workshop {

namespace {

constexpr size_t kMaxProjectBytes = 1024 * 1024;
constexpr size_t kMaxVdfBytes = 1024 * 1024;

// Wallpaper Engine's app id on Steam, the name of its folder of Workshop downloads.
constexpr wchar_t kWorkshop[] = L"\\steamapps\\workshop\\content\\431960";
constexpr wchar_t kMine[] = L"\\steamapps\\common\\wallpaper_engine\\projects\\myprojects";

// What Wallpaper Engine plays as a video wallpaper, and what it saves previews as.
const wchar_t *const kVideoExtensions[] = {L"mp4", L"m4v", L"webm", L"mkv", L"mov", L"avi", L"wmv"};
const wchar_t *const kPictureExtensions[] = {L"jpg", L"jpeg", L"png", L"gif", L"webp", L"bmp"};

std::wstring Lower(std::wstring text) {
    for (wchar_t &c : text) c = (wchar_t)std::towlower(c);
    return text;
}

template <size_t N>
bool HasExtension(const std::wstring &name, const wchar_t *const (&list)[N]) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = Lower(name.substr(dot + 1));
    for (const wchar_t *e : list)
        if (ext == e) return true;
    return false;
}

// A file name with no way out of the directory it is in: no separators, no drive, not . or ..
bool PlainName(const std::wstring &name) {
    if (name.empty() || name == L"." || name == L".." || name.size() > 255) return false;
    for (wchar_t c : name)
        if (c < 0x20 || c == L'\\' || c == L'/' || c == L':') return false;
    return true;
}

// One line of text: control characters (a title may hold line breaks) become spaces.
std::wstring OneLine(const std::wstring &text) {
    std::wstring out;
    for (wchar_t c : text) out += (c < 0x20 || c == 0x7F) ? L' ' : c;
    return std::wstring(Trim(out));
}

// The path as one spelling, to see that two name the same folder.
std::wstring Canonical(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// A file's size, or false when it is not a file.
bool FileSize(const std::wstring &path, uint64_t *bytes) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    *bytes = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

std::wstring RegistryText(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t text[MAX_PATH * 2];
    DWORD size = sizeof(text);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS) return L"";
    return text;
}

}  // namespace

std::vector<std::wstring> LibraryFolders(std::string_view vdf) {
    // VDF is quoted strings (with \\ and \" escapes), braces and // comments. A string followed
    // by a string is a key and its value; a string followed by a brace opens a block.
    struct Token {
        bool text;
        std::string value;  // the text, or "{" / "}"
    };
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
    std::vector<std::wstring> folders;
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].text) {
            depth += tokens[i].value == "{" ? 1 : -1;
            continue;
        }
        if (i + 1 >= tokens.size() || !tokens[i + 1].text) continue;  // a block's name
        const std::string &key = tokens[i].value;
        const bool numbered = !key.empty() && std::all_of(key.begin(), key.end(), [](char k) { return k >= '0' && k <= '9'; });
        if (_stricmp(key.c_str(), "path") == 0 || (depth == 1 && numbered)) {
            const std::wstring path = Canonical(FromUtf8(tokens[i + 1].value));
            if (!path.empty()) folders.push_back(path);
        }
        ++i;  // the value
    }
    return folders;
}

std::vector<Folder> FoldersUnder(const std::vector<std::wstring> &libraries) {
    std::vector<Folder> folders;
    std::vector<std::wstring> seen;
    auto add = [&](const std::wstring &path, Source source) {
        const std::wstring key = Lower(Canonical(path));
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || !IsDirectory(path)) return;
        seen.push_back(key);
        folders.push_back({Canonical(path), source});
    };
    for (const std::wstring &library : libraries) {
        const std::wstring root = Canonical(library);
        add(root + kWorkshop, Source::Workshop);
        add(root + kMine, Source::Mine);
    }
    return folders;
}

bool ReadProject(std::string_view text, const std::wstring &dir, Item *item, bool *project) {
    *project = false;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    json::Value v;
    if (!json::Parse(FromUtf8(text), &v) || !v.IsObject()) return false;
    *project = true;
    // Wallpaper Engine writes the type as "video" or "Video".
    if (Lower(v[L"type"].Str()) != L"video") return false;
    const std::wstring video = v[L"file"].Str();
    if (!PlainName(video) || !HasExtension(video, kVideoExtensions)) return false;
    const std::wstring preview = v[L"preview"].Str();
    item->dir = dir;
    item->video = video;
    item->preview = PlainName(preview) && HasExtension(preview, kPictureExtensions) ? preview : L"";
    item->title = OneLine(v[L"title"].Str());
    if (item->title.empty()) item->title = dir;
    return true;
}

Found Scan(const std::vector<Folder> &folders) {
    Found found;
    found.folders = folders;
    for (size_t f = 0; f < folders.size(); ++f) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW((folders[f].path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring dir = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dir == L"." || dir == L"..") continue;
            const std::wstring at = folders[f].path + L"\\" + dir;
            std::vector<uint8_t> bytes;
            if (!secure::ReadFileBytes(at + L"\\project.json", &bytes, kMaxProjectBytes)) continue;
            Item item;
            bool project = false;
            if (!ReadProject(std::string_view((const char *)bytes.data(), bytes.size()), dir, &item, &project)) {
                if (project) ++found.others;
                continue;
            }
            // Wallpaper Engine shows a project whose file is still downloading; it cannot be imported yet.
            if (!FileSize(at + L"\\" + item.video, &item.bytes) || item.bytes == 0) continue;
            uint64_t ignored = 0;
            if (!item.preview.empty() && !FileSize(at + L"\\" + item.preview, &ignored)) item.preview.clear();
            item.folder = f;
            item.id = std::to_wstring(f) + L"/" + dir;
            found.videos.push_back(std::move(item));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::stable_sort(found.videos.begin(), found.videos.end(), [](const Item &a, const Item &b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.title.c_str(), -1,
                               b.title.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return found;
}

std::vector<Folder> Folders() {
    std::wstring steam = RegistryText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return {};
    steam = Canonical(steam);
    // Steam itself is the first library; the others are listed in its libraryfolders.vdf (in
    // steamapps\ since 2021, in config\ as well).
    std::vector<std::wstring> libraries{steam};
    for (const wchar_t *list : {L"\\steamapps\\libraryfolders.vdf", L"\\config\\libraryfolders.vdf"}) {
        std::vector<uint8_t> bytes;
        if (!secure::ReadFileBytes(steam + list, &bytes, kMaxVdfBytes)) continue;
        for (const std::wstring &l : LibraryFolders(std::string_view((const char *)bytes.data(), bytes.size())))
            libraries.push_back(l);
    }
    return FoldersUnder(libraries);
}

}  // namespace app::workshop
