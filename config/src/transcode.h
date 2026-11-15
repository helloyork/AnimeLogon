// Turns a user's video into a video wallpaper as the logon screen plays it: H.264 MP4, 48 kHz
// stereo PCM WAV and wallpaper.ini (wallpaper.h). Runs unelevated, so no decoder of user files
// ever runs as SYSTEM.
//
// The video is converted on the graphics card where it has an H.264 encoder (NVENC, Quick Sync,
// AMF, Qualcomm's) through Media Foundation, decoding there too where the card can; frames stay in
// video memory from the decoder to the encoder. Where there is no such encoder (virtual machines,
// for one), where it cannot be set up, or where it fails partway, the whole video is converted
// again on the processor, as before. Either way the output is the same kind of file.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "animelogon/wallpaper.h"

namespace transcode {

struct Result {
    bool ok = false;
    std::wstring error;  // one line, for the settings app
    animelogon::WallpaperInfo info;  // kind video; no id and no sha256, which the elevated side fills in
    bool hardware = false;  // the video was encoded by the graphics card's encoder
};

// Where the video's bytes are: a file of its own (`path`), or a stretch of a file that is already
// open (`file`, from `offset`, `size` bytes), such as a stored entry of a theme package, which is
// then read where it lies instead of being copied out first. The handle must stay open, and no one
// able to write the file, until Run returns.
struct Source {
    std::wstring path;                      // the file, or the file the stretch is in (for the log)
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t offset = 0, size = 0;
    std::wstring name;                      // what wallpaper.ini records as the source; the file name when empty
};

enum class Path {
    Auto,      // the graphics card's encoder where there is one, the processor when it cannot be used
    Cpu,       // the processor only
    Hardware,  // the graphics card only, failing rather than falling back (for tests and tools)
};

struct Options {
    Path path = Path::Auto;
};

// `outputDir` receives video.mp4, audio.wav (when the source has sound) and wallpaper.ini, and
// nothing else. `progress` receives 0..1 (starting again from 0 if the conversion has to start
// again on the processor); `cancel` is checked between samples. Call with COM initialised.
Result Run(const std::wstring &sourcePath, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress);
Result Run(const Source &source, const std::wstring &outputDir, const std::wstring &displayName,
           const std::atomic<bool> &cancel, const std::function<void(double)> &progress, const Options &options = {});

// The graphics cards' H.264 encoders Media Foundation offers on this machine, by name, the one
// Run uses first. Call with COM initialised and Media Foundation started.
std::vector<std::wstring> HardwareEncoders();

// --- The decisions Run makes, for the tests ---

// Bits per second for the H.264 track: about 0.12 bits per pixel per frame, within 4..60 Mbit/s.
UINT32 Bitrate(UINT32 width, UINT32 height, double fps);
// Frames from one IDR picture to the next: one second's worth, as Media Foundation's own encoder
// has always written.
UINT32 KeyFrameInterval(UINT32 fpsNum, UINT32 fpsDen);
// Whether to start on the graphics card.
bool StartsOnHardware(Path path, bool hasHardwareEncoder);
// After the graphics card's attempt failed: whether to convert again on the processor. Not after a
// cancel, nor after a fault in the source itself that the processor would meet as well.
bool RetriesOnCpu(Path path, bool cancelled, bool sourceFault);

}  // namespace transcode
