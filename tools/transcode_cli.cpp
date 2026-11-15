// Runs the settings app's transcoder from the command line, for testing it against real
// videos:
//   transcode_cli [--cpu | --hardware] <source> <output dir>
//   transcode_cli --list
// The output directory receives a video wallpaper as the importer stages it: video.mp4,
// audio.wav when the source has sound, and wallpaper.ini. The transcoder's log lines go to
// <output dir>.log and are printed at the end. --cpu converts on the processor only; --hardware
// on the graphics card only, failing instead of falling back. --list names the graphics cards'
// H.264 encoders the transcoder would use.
#include <windows.h>
#include <mfapi.h>
#include <psapi.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "../config/src/transcode.h"
#include "animelogon/log.h"

#pragma comment(lib, "psapi.lib")

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    transcode::Options options;
    int at = 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--list") {
        const auto encoders = transcode::HardwareEncoders();
        if (encoders.empty()) std::wprintf(L"no graphics card's H.264 encoder\n");
        for (const std::wstring &e : encoders) std::wprintf(L"%s\n", e.c_str());
        return 0;
    }
    if (argc == 4 && std::wstring(argv[1]) == L"--cpu") options.path = transcode::Path::Cpu, at = 2;
    else if (argc == 4 && std::wstring(argv[1]) == L"--hardware") options.path = transcode::Path::Hardware, at = 2;
    else if (argc != 3) {
        std::fwprintf(stderr, L"usage: transcode_cli [--cpu | --hardware] <source> <output dir>\n       transcode_cli --list\n");
        return 2;
    }
    const std::wstring source = argv[at], out = argv[at + 1];
    const std::wstring logPath = out + L".log";
    DeleteFileW(logPath.c_str());
    animelogon::log::Open(logPath);
    CreateDirectoryW(out.c_str(), nullptr);
    std::atomic<bool> cancel{false};
    int last = -1;
    const ULONGLONG start = GetTickCount64();
    transcode::Source s;
    s.path = source;
    const transcode::Result r = transcode::Run(s, out, L"test", cancel, [&](double f) {
        const int pct = (int)(f * 100);
        if (pct / 10 != last / 10) std::fwprintf(stderr, L"%d%% ", pct);
        last = pct;
    }, options);
    const double secs = (GetTickCount64() - start) / 1000.0;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory));
    MFShutdown();
    CoUninitialize();
    if (FILE *log = _wfopen(logPath.c_str(), L"rt, ccs=UTF-8")) {
        std::fwprintf(stderr, L"\n");
        wchar_t line[2048];
        while (fgetws(line, ARRAYSIZE(line), log)) std::fwprintf(stderr, L"  %s", line);
        fclose(log);
    }
    std::wprintf(L"peak memory: %.0f MB (working set), %.0f MB (committed)\n", memory.PeakWorkingSetSize / 1048576.0,
                 memory.PeakPagefileUsage / 1048576.0);
    if (!r.ok) {
        std::fwprintf(stderr, L"failed after %.1f s: %s\n", secs, r.error.c_str());
        return 1;
    }
    std::wprintf(L"ok in %.1f s on the %s: %dx%d, %u/%u fps, %lld ms, audio %s; wrote video.mp4%s and wallpaper.ini\n", secs,
                 r.hardware ? L"graphics card" : L"processor", r.info.width, r.info.height, r.info.frameRateNum,
                 r.info.frameRateDen, r.info.durationMs, r.info.hasAudio ? L"yes" : L"no", r.info.hasAudio ? L", audio.wav" : L"");
    return 0;
}
