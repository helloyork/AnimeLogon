#include "animelogon/wallpaper.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxInfoBytes = 64 * 1024;
constexpr long long kMaxVideoSide = 16384;

// Keeps a name to one line, without control characters.
std::wstring OneLine(const std::wstring &text) {
    std::wstring out;
    for (wchar_t c : text) out += (c < 32 || c == 127) ? L' ' : c;
    return std::wstring(Trim(out));
}

bool IsLowerHex(std::wstring_view text, size_t digits) {
    if (text.size() != digits) return false;
    for (wchar_t c : text)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

bool IsDirectory(const std::wstring &path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// The size of a payload file the overlay may read: administrators-only and not empty.
bool TrustedPayload(const std::wstring &path, uint64_t *bytes, std::wstring *why) {
    std::wstring reason;
    if (!secure::IsTrusted(path, &reason)) {
        *why = path.substr(path.find_last_of(L'\\') + 1) + L" " + reason;
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !(data.nFileSizeHigh || data.nFileSizeLow)) {
        *why = path.substr(path.find_last_of(L'\\') + 1) + L" is missing or empty";
        return false;
    }
    *bytes += ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return true;
}

bool LoadStored(const std::wstring &id, WallpaperInfo *info, std::wstring *why) {
    const std::wstring ini = WallpaperInfoPath(id);
    std::wstring reason;
    std::vector<uint8_t> raw;
    if (!secure::IsTrusted(ini, &reason)) return *why = L"wallpaper.ini " + reason, false;
    if (!secure::ReadFileBytes(ini, &raw, kMaxInfoBytes)) return *why = L"wallpaper.ini could not be read", false;
    std::string_view bytes((const char *)raw.data(), raw.size());
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
    WallpaperInfo w;
    if (!ParseWallpaperInfo(FromUtf8(bytes), &w, &reason)) return *why = L"wallpaper.ini " + reason, false;
    w.id = id;
    if (w.kind == WallpaperKind::Image) {
        w.imagePath = WallpaperImagePath(id);
        if (!TrustedPayload(w.imagePath, &w.bytes, why)) return false;
    } else {
        w.videoPath = WallpaperVideoPath(id);
        if (!TrustedPayload(w.videoPath, &w.bytes, why)) return false;
        if (w.hasAudio) {
            w.audioPath = WallpaperAudioPath(id);
            if (!TrustedPayload(w.audioPath, &w.bytes, why)) return false;
        }
    }
    *info = std::move(w);
    return true;
}

WallpaperInfo BuiltIn() {
    WallpaperInfo w;
    w.id = kDefaultWallpaper;
    w.kind = WallpaperKind::Image;
    w.name = L"默认背景";
    w.sourceName = L"AnimeLogon";
    w.builtIn = true;
    return w;
}

}  // namespace

bool IsWallpaperId(std::wstring_view id) { return IsLowerHex(id, 16); }

bool IsWallpaperRef(std::wstring_view ref) {
    return ref == kDefaultWallpaper || ref == kNoWallpaper || IsWallpaperId(ref);
}

std::wstring NewWallpaperId() { return RandomHex(8); }

std::wstring WallpapersDir() { return paths::DataDir() + L"\\wallpapers"; }
std::wstring WallpaperDir(const std::wstring &id) { return WallpapersDir() + L"\\" + id; }
std::wstring WallpaperInfoPath(const std::wstring &id) { return WallpaperDir(id) + L"\\wallpaper.ini"; }
std::wstring WallpaperVideoPath(const std::wstring &id) { return WallpaperDir(id) + L"\\video.mp4"; }
std::wstring WallpaperAudioPath(const std::wstring &id) { return WallpaperDir(id) + L"\\audio.wav"; }
std::wstring WallpaperImagePath(const std::wstring &id) { return WallpaperDir(id) + L"\\image.bmp"; }

std::wstring SerializeWallpaperInfo(const WallpaperInfo &w) {
    const bool video = w.kind == WallpaperKind::Video;
    std::wstring out = Format(L"type = %s\r\nname = %s\r\nsource = %s\r\nimported = %s\r\nwidth = %d\r\nheight = %d\r\n",
                              video ? L"video" : L"image", OneLine(w.name).c_str(), OneLine(w.sourceName).c_str(),
                              OneLine(w.importedAt).c_str(), w.width, w.height);
    if (video)
        out += Format(L"frame_rate = %u/%u\r\nduration_ms = %lld\r\naudio = %s\r\n", w.frameRateNum, w.frameRateDen,
                      w.durationMs, w.hasAudio ? L"true" : L"false");
    if (!w.sha256.empty()) out += L"sha256 = " + w.sha256 + L"\r\n";
    return out;
}

bool ParseWallpaperInfo(const std::wstring &text, WallpaperInfo *info, std::wstring *why) {
    auto fail = [&](const wchar_t *what) {
        if (why) *why = what;
        return false;
    };
    WallpaperInfo w;
    bool haveType = false, haveWidth = false, haveHeight = false, haveRate = false, haveDuration = false;
    bool badHash = false;
    std::wstring type;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring_view line = Trim(std::wstring_view(text).substr(start, end - start));
        start = end + 1;
        const size_t eq = line.find(L'=');
        if (line.empty() || line[0] == L'#' || eq == std::wstring_view::npos) continue;
        const std::wstring_view key = Trim(line.substr(0, eq));
        const std::wstring_view value = Trim(line.substr(eq + 1));
        long long n = 0;
        if (key == L"type") type = value, haveType = true;
        else if (key == L"name") w.name = OneLine(std::wstring(value));
        else if (key == L"source") w.sourceName = OneLine(std::wstring(value));
        else if (key == L"imported") w.importedAt = OneLine(std::wstring(value));
        else if (key == L"width" && ParseInt(value, &n) && n > 0 && n <= kMaxVideoSide) w.width = (int)n, haveWidth = true;
        else if (key == L"height" && ParseInt(value, &n) && n > 0 && n <= kMaxVideoSide) w.height = (int)n, haveHeight = true;
        else if (key == L"duration_ms" && ParseInt(value, &n) && n > 0) w.durationMs = n, haveDuration = true;
        else if (key == L"audio") w.hasAudio = value == L"true";
        else if (key == L"sha256") {
            badHash = !IsLowerHex(value, 64);
            if (!badHash) w.sha256 = value;
        } else if (key == L"frame_rate") {
            const size_t slash = value.find(L'/');
            long long num = 0, den = 0;
            if (slash != std::wstring_view::npos && ParseInt(value.substr(0, slash), &num) &&
                ParseInt(value.substr(slash + 1), &den) && num > 0 && den > 0 && num <= 1000000 && den <= 1000000) {
                w.frameRateNum = (unsigned)num;
                w.frameRateDen = (unsigned)den;
                haveRate = true;
            }
        }
    }
    if (!haveType) return fail(L"has no type");
    if (type == L"scene") return fail(L"is a scene wallpaper, which this version cannot show");
    if (type == L"image") w.kind = WallpaperKind::Image;
    else if (type == L"video") w.kind = WallpaperKind::Video;
    else return fail(L"has an unknown type");
    if (!haveWidth || !haveHeight) return fail(L"has no valid width and height");
    if (badHash) return fail(L"has a bad sha256");
    if (w.kind == WallpaperKind::Image) {
        if (w.width > kMaxImageSide || w.height > kMaxImageSide) return fail(L"describes an image that is too large");
        w.frameRateNum = 0;
        w.frameRateDen = 1;
        w.durationMs = 0;
        w.hasAudio = false;
    } else if (!haveRate || !haveDuration) {
        return fail(L"has no valid frame rate and duration");
    }
    *info = std::move(w);
    return true;
}

bool IsCanonicalWav(const uint8_t *h, size_t headerBytes, uint64_t fileBytes) {
    if (!h || headerBytes < kWavHeaderBytes || fileBytes <= kWavHeaderBytes || fileBytes - 8 > 0xFFFFFFFFull)
        return false;
    const auto u16 = [&](size_t at) { return (uint32_t)h[at] | ((uint32_t)h[at + 1] << 8); };
    const auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
    const uint64_t data = fileBytes - kWavHeaderBytes;
    return !memcmp(h, "RIFF", 4) && u32(4) == fileBytes - 8 && !memcmp(h + 8, "WAVEfmt ", 8) && u32(16) == 16 &&
           u16(20) == 1 && u16(22) == 2 && u32(24) == 48000 && u32(28) == 48000 * 4 && u16(32) == 4 &&
           u16(34) == 16 && !memcmp(h + 36, "data", 4) && u32(40) == data && data % 4 == 0;
}

bool LooksLikeMp4(const uint8_t *h, size_t n) {
    if (!h || n < 12) return false;
    const uint32_t size = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    return !memcmp(h + 4, "ftyp", 4) && size >= 12 && size <= 4096;
}

bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) {
    std::wstring reason;
    if (id == kDefaultWallpaper) {
        *info = BuiltIn();
        return true;
    }
    if (!IsWallpaperId(id)) reason = L"not a wallpaper id";
    else if (!IsDirectory(WallpaperDir(id))) reason = L"no such wallpaper";
    else if (LoadStored(id, info, &reason)) return true;
    if (why) *why = reason;
    return false;
}

std::vector<WallpaperInfo> ListWallpapers() {
    std::vector<WallpaperInfo> out{BuiltIn()};
    auto byAge = [](const WallpaperInfo &a, const WallpaperInfo &b) { return a.importedAt < b.importedAt; };
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((WallpapersDir() + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !IsWallpaperId(fd.cFileName))
                continue;
            WallpaperInfo w;
            std::wstring why;
            if (LoadStored(fd.cFileName, &w, &why)) out.push_back(std::move(w));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(out.begin() + 1, out.end(), byAge);
    return out;
}

}  // namespace animelogon
