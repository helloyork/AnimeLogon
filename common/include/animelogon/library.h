// The imported videos: %ProgramData%\AnimeLogon\videos\<id>\{video.mp4, audio.wav, info.ini}.
// Every entry is the output of the settings app's transcoder, never the user's original file.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace animelogon {

struct VideoInfo {
    std::wstring id;
    std::wstring name;        // shown in the settings app
    std::wstring sourceName;  // the imported file's name, for reference
    std::wstring importedAt;  // ISO 8601, local time
    int width = 0;
    int height = 0;
    unsigned frameRateNum = 0;
    unsigned frameRateDen = 1;
    long long durationMs = 0;
    bool hasAudio = false;
    uint64_t bytes = 0;  // video + audio on disk; filled in by LoadVideo
};

// Library ids are 16 lowercase hex digits.
bool IsVideoId(std::wstring_view id);
std::wstring NewVideoId();

std::wstring VideoDir(const std::wstring &id);
std::wstring VideoFilePath(const std::wstring &id);
std::wstring AudioFilePath(const std::wstring &id);
std::wstring InfoFilePath(const std::wstring &id);

std::wstring SerializeVideoInfo(const VideoInfo &info);
bool ParseVideoInfo(const std::wstring &text, VideoInfo *info);

// audio.wav as the transcoder writes it: a 44-byte header (PCM, 48 kHz, 16-bit, stereo)
// followed by exactly the data it declares, and nothing else.
constexpr size_t kWavHeaderBytes = 44;
bool IsCanonicalWav(const uint8_t *header, size_t headerBytes, uint64_t fileBytes);
// video.mp4 starts with an ftyp box.
bool LooksLikeMp4(const uint8_t *head, size_t headBytes);

bool LoadVideo(const std::wstring &id, VideoInfo *info);
std::vector<VideoInfo> ListVideos();

}  // namespace animelogon
