#pragma once
#include <windows.h>
#include <cstdint>
namespace animelogon::scene::ipc {
// The parent and scene.exe are installed together; a renderer refuses a command of another version.
constexpr uint32_t kVersion = 5;
// The words of the state data, in this order, as the command carries them.
constexpr const wchar_t *kBatteryStates[] = {L"none", L"charging", L"discharging", L"plugged"};
constexpr const wchar_t *kPowerSources[] = {L"none", L"battery", L"mains"};
constexpr const wchar_t *kMediaStates[] = {L"none", L"playing", L"paused"};
struct Command {
    uint32_t version = kVersion, width = 0, height = 0, inputBytes = 0, outputBytes = 0, importing = 0;
    LUID adapter{};
    // How the scene is drawn (settings.h RenderSettings): the quality, the video memory limit in MB
    // (0: chosen from the graphics card) and the particle density in percent. For the automatic
    // quality, the tier the import trial chose.
    uint32_t quality = 0, videoMemory = 0, particles = 100;
    uint32_t calibrated = 0, startFps = 60;
    float startScale = 1;
    volatile LONG stop = 0, running = 0, sequence = 0;
    volatile LONG redraw = 0;
    volatile LONG videoSequence = 0;
    // Written between two increments of `sequence`: odd while the parent writes.
    float fps = 60;  // the frame rate cap for now, frames a second
    double time = 0;
    float day = 0, wake = 0, battery = -1, progress = -1;
    float audio[4]{};
    uint32_t month = 1, batteryState = 0, powerSource = 0, mediaState = 0;
    uint32_t valuesBytes = 0;
    char values[16384]{};
};
struct Status {
    volatile LONG state = 0, frames = 0;
    volatile LONG sequence = 0;
    HRESULT result = S_OK;
    uint32_t normalizedBytes = 0;
    float milliseconds = 0, scale = 1, fps = 60;
    // The video memory the scene's textures take, and how many mipmap levels were left out of
    // them to keep within the limit.
    uint64_t textureBytes = 0;
    uint32_t reducedLevels = 0;
    wchar_t error[512]{};
};
}  // namespace animelogon::scene::ipc
