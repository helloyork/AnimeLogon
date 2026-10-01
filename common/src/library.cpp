#include "animelogon/library.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxInfoBytes = 64 * 1024;

uint64_t FileBytes(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

// Keeps a name to one line, without control characters.
std::wstring OneLine(const std::wstring &text) {
    std::wstring out;
    for (wchar_t c : text) out += (c < 32 || c == 127) ? L' ' : c;
    return std::wstring(Trim(out));
}

}  // namespace

bool IsVideoId(std::wstring_view id) {
    if (id.size() != 16) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

std::wstring NewVideoId() { return RandomHex(8); }

std::wstring VideoDir(const std::wstring &id) { return paths::LibraryDir() + L"\\" + id; }
std::wstring VideoFilePath(const std::wstring &id) { return VideoDir(id) + L"\\video.mp4"; }
std::wstring AudioFilePath(const std::wstring &id) { return VideoDir(id) + L"\\audio.wav"; }
std::wstring InfoFilePath(const std::wstring &id) { return VideoDir(id) + L"\\info.ini"; }

std::wstring SerializeVideoInfo(const VideoInfo &v) {
    return Format(L"name = %s\r\nsource = %s\r\nimported = %s\r\nwidth = %d\r\nheight = %d\r\n"
                  L"frame_rate = %u/%u\r\nduration_ms = %lld\r\naudio = %s\r\n",
                  OneLine(v.name).c_str(), OneLine(v.sourceName).c_str(), OneLine(v.importedAt).c_str(),
                  v.width, v.height, v.frameRateNum, v.frameRateDen, v.durationMs,
                  v.hasAudio ? L"true" : L"false");
}

bool ParseVideoInfo(const std::wstring &text, VideoInfo *v) {
    bool haveSize = false, haveRate = false, haveDuration = false;
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
        if (key == L"name") v->name = value;
        else if (key == L"source") v->sourceName = value;
        else if (key == L"imported") v->importedAt = value;
        else if (key == L"width" && ParseInt(value, &n) && n > 0 && n <= 16384) v->width = (int)n;
        else if (key == L"height" && ParseInt(value, &n) && n > 0 && n <= 16384) v->height = (int)n, haveSize = true;
        else if (key == L"duration_ms" && ParseInt(value, &n) && n > 0) v->durationMs = n, haveDuration = true;
        else if (key == L"audio") v->hasAudio = value == L"true";
        else if (key == L"frame_rate") {
            const size_t slash = value.find(L'/');
            long long num = 0, den = 0;
            if (slash != std::wstring_view::npos && ParseInt(value.substr(0, slash), &num) &&
                ParseInt(value.substr(slash + 1), &den) && num > 0 && den > 0 && num <= 1000000 && den <= 1000000) {
                v->frameRateNum = (unsigned)num;
                v->frameRateDen = (unsigned)den;
                haveRate = true;
            }
        }
    }
    return haveSize && v->width > 0 && haveRate && haveDuration;
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

bool LoadVideo(const std::wstring &id, VideoInfo *info) {
    if (!IsVideoId(id)) return false;
    std::vector<uint8_t> bytes;
    if (!secure::ReadFileBytes(InfoFilePath(id), &bytes, kMaxInfoBytes)) return false;
    VideoInfo v;
    v.id = id;
    if (!ParseVideoInfo(FromUtf8(std::string_view((const char *)bytes.data(), bytes.size())), &v)) return false;
    v.bytes = FileBytes(VideoFilePath(id));
    if (!v.bytes) return false;
    if (v.hasAudio) v.bytes += FileBytes(AudioFilePath(id));
    *info = v;
    return true;
}

std::vector<VideoInfo> ListVideos() {
    std::vector<VideoInfo> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((paths::LibraryDir() + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        VideoInfo v;
        if (LoadVideo(fd.cFileName, &v)) out.push_back(std::move(v));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const VideoInfo &a, const VideoInfo &b) { return a.importedAt < b.importedAt; });
    return out;
}

}  // namespace animelogon
