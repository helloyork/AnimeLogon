#pragma once
#include <windows.h>
#include <cstdint>
namespace animelogon::scene::ipc {
constexpr uint32_t kVersion = 3;
struct Command {
    uint32_t version = kVersion, width = 0, height = 0, inputBytes = 0, outputBytes = 0, importing = 0;
    LUID adapter{};
    uint32_t calibrated = 0, startFps = 60;
    float startScale = 1;
    volatile LONG stop = 0, running = 0, sequence = 0;
    volatile LONG redraw = 0;
    float time = 0, day = 0, battery = -1, progress = 0, playing = 0, refresh = 60;
    uint32_t valuesBytes = 0;
    char values[16384]{};
};
struct Status {
    volatile LONG state = 0, frames = 0;
    volatile LONG sequence = 0;
    HRESULT result = S_OK;
    uint32_t normalizedBytes = 0;
    float milliseconds = 0, scale = 1, fps = 60;
    float bounds[8][4]{};
    wchar_t error[512]{};
};
}  // namespace animelogon::scene::ipc
