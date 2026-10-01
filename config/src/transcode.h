// Turns a user's video into what the logon screen plays: H.264 MP4, 48 kHz stereo PCM WAV and
// info.ini. Runs unelevated, so no decoder of user files ever runs as SYSTEM.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "animelogon/library.h"

namespace transcode {

struct Result {
    bool ok = false;
    std::wstring error;        // one line, for the settings app
    animelogon::VideoInfo info;
};

// `outputDir` receives video.mp4, audio.wav (when the source has sound) and info.ini.
// `progress` receives 0..1; `cancel` is checked between samples. Call with COM initialised.
Result Run(const std::wstring &sourcePath, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress);

}  // namespace transcode
