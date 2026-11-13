// Draws a skin over a still image, for judging its look without a lock screen:
// skin_preview <background> <out.png> [--skin skin.xml] [--locale ll-CC] [--time HH:MM] [--date YYYY-MM-DD]
//              [--hours auto|12|24] [--ampm on|off] [--date-style weekday|long|none] [--language ll-CC]
//              [--component battery|media|countdown] [--data sample|paused|charging|plugged|mains|idle]
//              [--media "title|artist|position|duration"]
//              [setting=value ...] [p1.l1.t1.weight=300 ...]
// Without --skin or --component it draws the default clock. --locale stands in for the regional
// format. Without --data there is no battery and nothing playing; with mains, a machine without a
// battery, on mains power, playing nothing; with idle, the sample battery and nothing playing; with
// charging and plugged, a battery on mains power, charging or full. --media says what is playing
// instead of the sample: its title, artist, and how far in and how long it is, in seconds (the last
// three may be left out).
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>

#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/data.h"
#include "animelogon/skin.h"
#include "animelogon/skinpreview.h"
#include "animelogon/text.h"

using namespace animelogon;

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
    std::wstring media;
    bool withMedia = false;
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
            if (kind == L"charging") snapshot.battery = {data::BatteryState::Charging, 42, -1, true};
            if (kind == L"plugged") snapshot.battery = {data::BatteryState::Plugged, 100, -1, true};
            if (kind == L"mains") snapshot = {{data::BatteryState::None, -1, -1, true}, {}};
            if (kind == L"idle") snapshot.media = {};
            withData = true;
        } else if (arg == L"--media" && more) {
            media = argv[++i];
            withMedia = true;
        } else if (arg.find(L'=') != std::wstring::npos) {
            const size_t eq = arg.find(L'=');
            values[arg.substr(0, eq)] = arg.substr(eq + 1);
        } else {
            std::fwprintf(stderr, L"unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    // What is playing, given on the command line: the sample's battery and app, this title and artist,
    // playing at this position. Applied after --data so the order of the two does not matter.
    if (withMedia) {
        if (!withData) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            snapshot = data::Sample((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
            withData = true;
        }
        std::wstring parts[4];
        size_t start = 0;
        for (int k = 0; k < 4; ++k) {
            const size_t bar = media.find(L'|', start);
            parts[k] = media.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        snapshot.media.state = data::MediaState::Playing;
        snapshot.media.title = parts[0];
        snapshot.media.artist = parts[1];
        snapshot.media.album.clear();
        snapshot.media.position = parts[2].empty() ? 0.0 : std::wcstod(parts[2].c_str(), nullptr);
        snapshot.media.duration = parts[3].empty() ? 0.0 : std::wcstod(parts[3].c_str(), nullptr);
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
    SkinPreview preview;
    if (!preview.Render(background, skin::Resolve(s, values), style, StandardFormat(locale), now, &out, nullptr, false,
                        withData ? &snapshot : nullptr)) {
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
