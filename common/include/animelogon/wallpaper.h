// Wallpapers: what fills a display behind the components. An imported wallpaper lives in
// %ProgramData%\AnimeLogon\wallpapers\<id>\ as wallpaper.ini plus either video.mp4 (and
// audio.wav when it has sound) or image.bmp. Every file there is the output of the settings
// app's normalisation, written only by its elevated helper; wallpaper.ini is parsed and written
// afresh there, never copied.
//
// "default" is the built-in wallpaper: the gradient the installer uses as the sign-in
// background. It has no files; the overlay draws it itself.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace animelogon {

// type=scene in wallpaper.ini is reserved for scene wallpapers and refused for now.
enum class WallpaperKind { Image, Video };

struct WallpaperInfo {
    std::wstring id;
    WallpaperKind kind = WallpaperKind::Video;
    std::wstring name;        // shown in the settings app
    std::wstring sourceName;  // the imported file's name, for reference
    std::wstring importedAt;  // ISO 8601, local time
    int width = 0;            // pixels, both kinds
    int height = 0;
    // Video only.
    unsigned frameRateNum = 0;
    unsigned frameRateDen = 1;
    long long durationMs = 0;
    bool hasAudio = false;
    // SHA-256 of the normalised payload as 64 lowercase hex digits: the bytes of image.bmp, or
    // of video.mp4 followed by those of audio.wav. The elevated helper fills it in and uses it to
    // find a wallpaper that is already installed. Empty when unknown.
    std::wstring sha256;

    // Filled in by LoadWallpaper; never stored in wallpaper.ini.
    std::wstring videoPath, audioPath, imagePath;  // empty where the wallpaper has no such file
    uint64_t bytes = 0;                            // the payload on disk
    bool builtIn = false;                          // "default": no files, drawn by AnimeLogon
};

constexpr const wchar_t *kDefaultWallpaper = L"default";
constexpr const wchar_t *kNoWallpaper = L"none";
// The longest side an image wallpaper may have; larger images are scaled down on import.
constexpr int kMaxImageSide = 7680;

// Imported wallpapers have ids of 16 lowercase hex digits.
bool IsWallpaperId(std::wstring_view id);
// What an installed theme's wallpaper may name: a wallpaper id, "default" or "none".
bool IsWallpaperRef(std::wstring_view ref);
std::wstring NewWallpaperId();

std::wstring WallpapersDir();
std::wstring WallpaperDir(const std::wstring &id);
std::wstring WallpaperInfoPath(const std::wstring &id);   // wallpaper.ini
std::wstring WallpaperVideoPath(const std::wstring &id);  // video.mp4
std::wstring WallpaperAudioPath(const std::wstring &id);  // audio.wav
std::wstring WallpaperImagePath(const std::wstring &id);  // image.bmp

// wallpaper.ini: one `key = value` per line, unknown keys ignored. `type`, `width` and `height`
// are required, and for a video also `frame_rate` and `duration_ms`. `why` says what was wrong.
std::wstring SerializeWallpaperInfo(const WallpaperInfo &info);
bool ParseWallpaperInfo(const std::wstring &text, WallpaperInfo *info, std::wstring *why = nullptr);

// audio.wav as the transcoder writes it: a 44-byte header (PCM, 48 kHz, 16-bit, stereo)
// followed by exactly the data it declares, and nothing else.
constexpr size_t kWavHeaderBytes = 44;
bool IsCanonicalWav(const uint8_t *header, size_t headerBytes, uint64_t fileBytes);
// video.mp4 starts with an ftyp box.
bool LooksLikeMp4(const uint8_t *head, size_t headBytes);

// The wallpaper `id` names, with its file paths filled in: the built-in "default", or an entry
// of wallpapers\ whose wallpaper.ini parses and whose files are administrators-only
// (secure::IsTrusted).
bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why = nullptr);

// The built-in wallpaper first, then the entries of wallpapers\ that load, oldest first.
std::vector<WallpaperInfo> ListWallpapers();

}  // namespace animelogon
