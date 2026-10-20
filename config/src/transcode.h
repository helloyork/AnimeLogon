// Turns a user's video into a video wallpaper as the logon screen plays it: H.264 MP4, 48 kHz
// stereo PCM WAV and wallpaper.ini (wallpaper.h). Runs unelevated, so no decoder of user files
// ever runs as SYSTEM.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "animelogon/wallpaper.h"

namespace transcode {

struct Result {
    bool ok = false;
    std::wstring error;  // one line, for the settings app
    animelogon::WallpaperInfo info;  // kind video; no id and no sha256, which the elevated side fills in
};

// `outputDir` receives video.mp4, audio.wav (when the source has sound) and wallpaper.ini, and
// nothing else. `progress` receives 0..1; `cancel` is checked between samples. Call with COM
// initialised.
Result Run(const std::wstring &sourcePath, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress);

}  // namespace transcode
