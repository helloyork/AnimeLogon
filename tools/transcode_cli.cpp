// Runs the settings app's transcoder from the command line, for testing it against real
// videos: transcode_cli <source> <output dir>
// The output directory receives a video wallpaper as the importer stages it: video.mp4,
// audio.wav when the source has sound, and wallpaper.ini.
#include <windows.h>
#include <mfapi.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdio>

#include "../config/src/transcode.h"

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: transcode_cli <source> <output dir>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    CreateDirectoryW(argv[2], nullptr);
    std::atomic<bool> cancel{false};
    int last = -1;
    const ULONGLONG start = GetTickCount64();
    const transcode::Result r = transcode::Run(argv[1], argv[2], L"test", cancel, [&](double f) {
        const int pct = (int)(f * 100);
        if (pct / 10 != last / 10) std::fwprintf(stderr, L"%d%% ", pct);
        last = pct;
    });
    const double secs = (GetTickCount64() - start) / 1000.0;
    MFShutdown();
    CoUninitialize();
    if (!r.ok) {
        std::fwprintf(stderr, L"\nfailed after %.1f s: %s\n", secs, r.error.c_str());
        return 1;
    }
    std::wprintf(L"\nok in %.1f s: %dx%d, %u/%u fps, %lld ms, audio %s; wrote video.mp4%s and wallpaper.ini\n", secs,
                 r.info.width, r.info.height, r.info.frameRateNum, r.info.frameRateDen, r.info.durationMs,
                 r.info.hasAudio ? L"yes" : L"no", r.info.hasAudio ? L", audio.wav" : L"");
    return 0;
}
