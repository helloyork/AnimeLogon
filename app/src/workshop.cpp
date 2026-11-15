#include "workshop.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "animelogon/secure.h"
#include "animelogon/text.h"

#include "json.h"
#include "steam_library.h"

using namespace animelogon;
using app::steam_library::Canonical;

namespace app::workshop {

namespace {

constexpr size_t kMaxProjectBytes = 1024 * 1024;

// Wallpaper Engine's app id on Steam, the name of its folder of Workshop downloads.
constexpr uint32_t kAppId = 431960;
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

}  // namespace

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
        add(steam_library::WorkshopContentPath(root, kAppId), Source::Workshop);
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

std::vector<Folder> Folders() { return FoldersUnder(steam_library::Libraries()); }

}  // namespace app::workshop
