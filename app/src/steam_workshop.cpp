#include "steam_workshop.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/sha256.h"
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"

#include "steam_client.h"

using namespace animelogon;

namespace app::steam_workshop {

namespace {

constexpr wchar_t kSection[] = L"installed ";
constexpr size_t kMaxRecordsBytes = 1024 * 1024;

// Seconds since 1970 (UTC) of a file time.
long long UnixTime(const FILETIME &ft) {
    const unsigned long long ticks = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    constexpr unsigned long long k1970 = 116444736000000000ull;  // 100 ns ticks from 1601 to 1970
    return ticks < k1970 ? 0 : (long long)((ticks - k1970) / 10000000ull);
}

bool EndsWithNoCase(const std::wstring &text, const wchar_t *suffix) {
    const size_t n = wcslen(suffix);
    return text.size() > n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

// One line of the file: what is between its start and a newline, a '\r' before it dropped.
std::wstring_view LineAt(std::wstring_view text, size_t *at) {
    const size_t end = text.find(L'\n', *at);
    std::wstring_view line = text.substr(*at, end == std::wstring_view::npos ? std::wstring_view::npos : end - *at);
    *at = end == std::wstring_view::npos ? text.size() : end + 1;
    if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
    return line;
}

// A title kept on one line of the file.
std::wstring OneLine(const std::wstring &text) {
    std::wstring out;
    for (wchar_t c : text) out += (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
    return std::wstring(Trim(out));
}

bool HashFile(Sha256 *hash, const std::wstring &path) {
    std::wstring why;
    HANDLE h = secure::OpenPlainFile(path, &why);
    if (h == INVALID_HANDLE_VALUE || !h) return false;
    const bool ok = hash->UpdateFromHandle(h);
    CloseHandle(h);
    return ok;
}

bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

// --- workshop.ini --------------------------------------------------------------------------------

std::wstring RecordsPath() { return paths::UserDataDir() + L"\\workshop.ini"; }

Records ParseRecords(std::wstring_view text) {
    if (!text.empty() && text[0] == 0xFEFF) text.remove_prefix(1);
    Records out;
    std::wstring item;  // the section being read, or empty
    Record record;
    auto finish = [&] {
        if (!item.empty() && IsWallpaperId(record.theme)) out[item] = record;
        item.clear();
        record = Record();
    };
    size_t at = 0;
    while (at < text.size()) {
        const std::wstring_view line = Trim(LineAt(text, &at));
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[') {
            finish();
            if (line.back() != L']') continue;
            const std::wstring_view name = Trim(line.substr(1, line.size() - 2));
            if (name.size() > wcslen(kSection) && name.compare(0, wcslen(kSection), kSection) == 0) {
                const std::wstring_view id = Trim(name.substr(wcslen(kSection)));
                if (steam::IsItemId(id)) item = std::wstring(id);
            }
            continue;
        }
        if (item.empty()) continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring_view::npos) continue;
        const std::wstring_view key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        long long n = 0;
        if (key == L"theme") record.theme = std::wstring(value);
        else if (key == L"title") record.title = std::wstring(value);
        else if (key == L"updated" && ParseInt(value, &n) && n >= 0) record.updated = n;
        else if (key == L"file" && ParseInt(value, &n) && n >= 0) record.file = n;
    }
    finish();
    return out;
}

std::wstring FormatRecords(const Records &records) {
    std::wstring out =
        L"; The Steam Workshop items installed as themes for this person, written by the AnimeLogon\r\n"
        L"; settings app. updated: Steam's time of the item's last update then; file: when its .altheme\r\n"
        L"; was last written then (seconds since 1970, UTC).\r\n";
    for (const auto &[id, r] : records) {
        if (!steam::IsItemId(id) || !IsWallpaperId(r.theme)) continue;
        out += L"\r\n[" + std::wstring(kSection) + id + L"]\r\n";
        out += L"theme = " + r.theme + L"\r\n";
        out += L"updated = " + std::to_wstring(r.updated) + L"\r\n";
        out += L"file = " + std::to_wstring(r.file) + L"\r\n";
        const std::wstring title = OneLine(r.title);
        if (!title.empty()) out += L"title = " + title + L"\r\n";
    }
    return out;
}

Records LoadRecords(const std::wstring &path) {
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(path, &bytes, kMaxRecordsBytes)) return {};
    return ParseRecords(FromUtf8(std::string_view((const char *)bytes.data(), bytes.size())));
}

bool SaveRecords(const std::wstring &path, const Records &records) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) paths::CreateDirectories(path.substr(0, slash));
    const std::string text = ToUtf8(FormatRecords(records));
    // Written beside it, then put in its place, so that a failure leaves the old file whole.
    const std::wstring temp = path + L".new";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool written = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    if (!CloseHandle(h) || !written || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// --- an item's folder ----------------------------------------------------------------------------

Folder Inspect(const std::wstring &dir) {
    Folder f;
    if (dir.empty()) return f;
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return f;
    f.contents = Contents::None;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return f;
    size_t packages = 0;
    bool plain = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || !EndsWithNoCase(name, L".altheme")) continue;
        ++packages;
        if (packages > 1) continue;
        plain = !(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE));
        f.package = dir + L"\\" + name;
        f.time = UnixTime(fd.ftLastWriteTime);
        f.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (packages == 0) f.contents = Contents::None;
    else if (packages > 1) f.contents = Contents::Several;
    else f.contents = plain ? Contents::Ok : Contents::NotPlain;
    if (f.contents != Contents::Ok) {
        f.package.clear();
        f.time = 0;
        f.size = 0;
    }
    return f;
}

const char *ProblemKey(Contents contents) {
    switch (contents) {
    case Contents::Missing: return "steam.problem.missing";
    case Contents::None: return "steam.problem.none";
    case Contents::Several: return "steam.problem.several";
    case Contents::NotPlain: return "steam.problem.not_plain";
    default: return "";
    }
}

// --- the items -----------------------------------------------------------------------------------

std::vector<Item> FromSubscribed(const json::Value &answer) {
    std::vector<Item> out;
    for (const json::Value &v : answer[L"items"].Items()) {
        Item item;
        item.id = v[L"id"].Str();
        if (!steam::IsItemId(item.id)) continue;
        const json::Value &state = v[L"state"];
        item.subscribed = state[L"subscribed"].Bool(true);
        item.installed = state[L"installed"].Bool();
        item.needsUpdate = state[L"needsUpdate"].Bool();
        item.downloading = state[L"downloading"].Bool() || state[L"downloadPending"].Bool();
        item.folder = v[L"folder"].Str();
        item.sizeOnDisk = (unsigned long long)std::max(0.0, v[L"sizeOnDisk"].Num());
        item.title = v[L"title"].Str();
        item.preview = v[L"preview"].Str();
        item.timeUpdated = (long long)std::max(0.0, v[L"timeUpdated"].Num());
        for (const json::Value &d : v[L"descriptors"].Items())
            if (d.IsString()) item.descriptors.push_back(d.Str());
        if (IsRating(v[L"rating"].Str())) item.rating = v[L"rating"].Str();
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Item> FromFolders(const std::vector<std::wstring> &contentDirs) {
    std::vector<Item> out;
    std::set<std::wstring> seen;
    for (const std::wstring &dir : contentDirs) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::wstring id = fd.cFileName;
            if (!steam::IsItemId(id) || !seen.insert(id).second) continue;
            Item item;
            item.id = id;
            item.folder = dir + L"\\" + id;
            item.subscribed = true;
            item.installed = true;
            out.push_back(std::move(item));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) {
        return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
    return out;
}

const wchar_t *NameOf(State state) {
    switch (state) {
    case State::Downloading: return L"downloading";
    case State::NotDownloaded: return L"notDownloaded";
    case State::Unreadable: return L"unreadable";
    case State::NotInstalled: return L"notInstalled";
    case State::Installed: return L"installed";
    case State::Update: return L"update";
    case State::Unsubscribed: return L"unsubscribed";
    }
    return L"";
}

State StateOf(const Item &item, const Folder &folder, const Record *record, bool themeInstalled) {
    const bool installed = record && themeInstalled;
    if (installed && !item.subscribed) return State::Unsubscribed;
    // Steam is to download it: as a new item, or a newer version of the files on this computer.
    if (item.downloading || (item.needsUpdate && item.installed)) return State::Downloading;
    if (!installed) {
        if (!item.installed || folder.contents == Contents::Missing) return State::NotDownloaded;
        return folder.contents == Contents::Ok ? State::NotInstalled : State::Unreadable;
    }
    // Newer than what was installed: by Steam's word when both times are known, otherwise by the
    // .altheme on this computer having been written since.
    bool newer = false;
    if (item.timeUpdated > 0 && record->updated > 0) newer = item.timeUpdated > record->updated;
    else if (folder.contents == Contents::Ok && record->file > 0) newer = folder.time != record->file;
    if (!newer || !item.installed || folder.contents == Contents::Missing) return State::Installed;
    return folder.contents == Contents::Ok ? State::Update : State::Unreadable;
}

// --- replacing a theme with its update -------------------------------------------------------------

std::wstring StagedWallpaperHash(const std::wstring &dir) {
    Sha256 hash;
    if (Exists(dir + L"\\scene.xml")) return L"";
    if (Exists(dir + L"\\image.bmp")) {
        if (!HashFile(&hash, dir + L"\\image.bmp")) return L"";
    } else if (Exists(dir + L"\\video.mp4")) {
        if (!HashFile(&hash, dir + L"\\video.mp4")) return L"";
        if (Exists(dir + L"\\audio.wav") && !HashFile(&hash, dir + L"\\audio.wav")) return L"";
    } else {
        return L"";
    }
    return hash.Finish();
}

std::string NormalizedComponent(const std::wstring &file) {
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(file, &bytes, 256 * 1024)) return "";
    skin::Skin parsed;
    std::wstring why;
    if (!skin::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &parsed, &why)) return "";
    return skin::Normalize(parsed);
}

Reuse StagedReuse(const std::vector<std::wstring> &dirs) {
    Reuse r;
    for (const std::wstring &dir : dirs) {
        if (Exists(dir + L"\\wallpaper")) {
            const std::wstring hash = StagedWallpaperHash(dir + L"\\wallpaper");
            if (hash.empty()) r.wallpapersKnown = false;
            else r.wallpapers.insert(hash);
        }
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dir + L"\\components\\*.xml").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string text = NormalizedComponent(dir + L"\\components\\" + fd.cFileName);
            if (text.empty()) r.componentsKnown = false;
            else r.components.insert(text);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return r;
}

// --- publishing ------------------------------------------------------------------------------------

bool IsRating(std::wstring_view name) {
    return std::any_of(std::begin(kRatings), std::end(kRatings), [&](const wchar_t *r) { return name == r; });
}

std::vector<std::wstring> TagsFor(const TagFacts &facts) {
    std::vector<std::wstring> tags{facts.video ? L"Video" : L"Picture"};
    if (facts.height >= 2160) tags.push_back(L"4K");
    else if (facts.height >= 1440) tags.push_back(L"1440p");
    else if (facts.height >= 1080) tags.push_back(L"1080p");
    // At least 2.2 times as wide as high, in whole numbers: width * 5 >= height * 11.
    if (facts.width > 0 && facts.height > 0 && (long long)facts.width * 5 >= (long long)facts.height * 11) tags.push_back(L"Ultrawide");
    if (facts.sound) tags.push_back(L"Sound");
    if (facts.components) tags.push_back(L"Components");
    if (facts.customComponents) tags.push_back(L"Custom components");
    if (IsRating(facts.rating)) tags.push_back(facts.rating);
    return tags;
}

bool CarriesOwnComponents(const std::vector<package::Entry> &entries) {
    return std::any_of(entries.begin(), entries.end(), [](const package::Entry &e) { return e.kind == package::Kind::Component; });
}

}  // namespace app::steam_workshop
