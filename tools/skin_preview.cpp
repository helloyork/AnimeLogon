// Draws a skin over a still image, for judging its look without a lock screen:
// skin_preview <background> <out.png> [--skin skin.xml] [--locale ll-CC] [--time HH:MM] [--date YYYY-MM-DD]
//              [--hours auto|12|24] [--ampm on|off] [--date-style weekday|long|none] [--language ll-CC]
//              [--component battery|media|countdown|spectrum] [--data sample|paused|charging|mains]
//              [--audio <sound.wav>|silence [--at seconds]]
//              [setting=value ...] [p1.l1.t1.weight=300 ...]
// Without --skin or --component it draws the default clock. --locale stands in for the regional
// format. Without --data there is no battery and nothing playing; with mains, a machine without a
// battery, on mains power, playing nothing. Spectra show a made-up sound unless --audio gives a
// WAV file (PCM or float, any rate) and where in it, or silence.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/data.h"
#include "animelogon/skin.h"
#include "animelogon/skinpreview.h"
#include "animelogon/spectrum.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace {

// The spectrum of a WAV file `seconds` in: 16-bit PCM or 32-bit float, mixed to one channel.
bool ReadSound(const wchar_t *path, double seconds, spectrum::Frame *out) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") || bytes.compare(8, 4, "WAVE")) return false;
    auto u16 = [&](size_t at) { return (unsigned)(unsigned char)bytes[at] | (unsigned)(unsigned char)bytes[at + 1] << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    unsigned format = 0, channels = 0, rate = 0, bits = 0;
    size_t data = 0, length = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const size_t size = u32(at + 4);
        if (!bytes.compare(at, 4, "fmt ") && size >= 16) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            bits = u16(at + 22);
            if (format == 0xFFFE && size >= 40) format = u16(at + 32);  // WAVE_FORMAT_EXTENSIBLE: its sub-format
        } else if (!bytes.compare(at, 4, "data")) {
            data = at + 8;
            length = std::min(size, bytes.size() - data);
        }
        at += 8 + size + (size & 1);
    }
    const bool pcm = format == 1 && bits == 16, real = format == 3 && bits == 32;
    if (!data || !channels || !rate || (!pcm && !real)) return false;
    const size_t frameBytes = channels * bits / 8, frames = length / frameBytes;
    const size_t want = spectrum::Analyzer::SizeFor((float)rate);
    const size_t end = std::min(frames, (size_t)(seconds * rate) + want), begin = end > want ? end - want : 0;
    std::vector<float> mono;
    for (size_t f = begin; f < end; ++f) {
        float sum = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            const size_t at = data + f * frameBytes + c * bits / 8;
            if (pcm) sum += (float)(int16_t)u16(at) / 32768.0f;
            else {
                float v = 0.0f;
                std::memcpy(&v, bytes.data() + at, 4);
                sum += v;
            }
        }
        mono.push_back(sum / (float)channels);
    }
    spectrum::Analyzer analyzer;
    analyzer.Run(mono.data(), mono.size(), (float)rate, seconds, out);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: skin_preview <background> <out.png> [--skin skin.xml] [--locale ll-CC] "
                              L"[--time HH:MM] [--date YYYY-MM-DD] [--hours auto|12|24] [--ampm on|off] "
                              L"[--date-style weekday|long|none] [--language ll-CC] [setting=value ...] "
                              L"[element.attribute=value ...]\n");
        return 2;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    skin::Skin s = skin::Default();
    skin::Values values;
    wchar_t user[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(user, ARRAYSIZE(user));
    std::wstring locale = user;
    ClockStyle style;
    bool withData = false;
    data::Snapshot snapshot;
    std::wstring sound;
    double soundAt = 30.0;
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool more = i + 1 < argc;
        if (arg == L"--time" && more) {
            swscanf_s(argv[++i], L"%hu:%hu", &now.wHour, &now.wMinute);
        } else if (arg == L"--date" && more) {
            swscanf_s(argv[++i], L"%hu-%hu-%hu", &now.wYear, &now.wMonth, &now.wDay);
            FILETIME ft;  // recomputes the weekday
            SystemTimeToFileTime(&now, &ft);
            FileTimeToSystemTime(&ft, &now);
        } else if (arg == L"--locale" && more) {
            locale = argv[++i];
        } else if (arg == L"--hours" && more) {
            Parse(argv[++i], &style.hours);
        } else if (arg == L"--ampm" && more) {
            style.ampm = std::wstring(argv[++i]) != L"off";
        } else if (arg == L"--date-style" && more) {
            Parse(argv[++i], &style.date);
        } else if (arg == L"--language" && more) {
            style.locale = argv[++i];
        } else if (arg == L"--skin" && more) {
            std::ifstream in(argv[++i], std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::wstring error;
            if (!skin::Parse(text, &s, &error)) {
                std::fwprintf(stderr, L"%s: %s\n", argv[i], error.c_str());
                return 1;
            }
        } else if (arg == L"--component" && more) {
            if (!BuiltInComponent(argv[++i], &s)) {
                std::fwprintf(stderr, L"no built-in component %s\n", argv[i]);
                return 2;
            }
        } else if (arg == L"--data" && more) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            snapshot = data::Sample((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
            const std::wstring kind = argv[++i];
            if (kind == L"paused") snapshot.media.state = data::MediaState::Paused;
            if (kind == L"charging") snapshot.battery = {data::BatteryState::Charging, 42, -1};
            if (kind == L"mains") snapshot = {{data::BatteryState::None, -1, -1, true}, {}};
            withData = true;
        } else if (arg == L"--audio" && more) {
            sound = argv[++i];
        } else if (arg == L"--at" && more) {
            soundAt = std::wcstod(argv[++i], nullptr);
        } else if (arg.find(L'=') != std::wstring::npos) {
            const size_t eq = arg.find(L'=');
            values[arg.substr(0, eq)] = arg.substr(eq + 1);
        } else {
            std::fwprintf(stderr, L"unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    for (const auto &[id, value] : values) {
        const skin::Setting *setting = s.Find(id);
        if (setting ? !skin::IsValue(*setting, value) : !skin::IsAdjustmentValue(id, value))
            std::fwprintf(stderr, L"%s=%s is not a value of this skin; using the default\n", id.c_str(), value.c_str());
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Picture background, out;
    if (!LoadPicture(argv[1], 0, &background)) {
        std::fwprintf(stderr, L"cannot read %s\n", argv[1]);
        return 1;
    }
    spectrum::Frame audio;  // silence unless read
    if (!sound.empty() && sound != L"silence" && !ReadSound(sound.c_str(), soundAt, &audio)) {
        std::fwprintf(stderr, L"cannot read %s as 16-bit PCM or 32-bit float WAV\n", sound.c_str());
        return 1;
    }
    SkinPreview preview;
    if (!preview.Render(background, skin::Resolve(s, values), style, StandardFormat(locale), now, &out, nullptr, false,
                        withData ? &snapshot : nullptr, sound.empty() ? nullptr : &audio)) {
        std::fwprintf(stderr, L"the skin could not be drawn\n");
        return 1;
    }
    if (!SavePicture(argv[2], out)) {
        std::fwprintf(stderr, L"cannot write %s\n", argv[2]);
        return 1;
    }
    std::wprintf(L"%s: %ux%u, %s, %s\n", argv[2], out.width, out.height, s.name.c_str(), locale.c_str());
    return 0;
}
