// Draws a skin over a still image, for judging its look without a lock screen:
// skin_preview <background> <out.png> [--skin skin.xml] [--locale ll-CC] [--time HH:MM] [--date YYYY-MM-DD]
//              [--hours auto|12|24] [--ampm on|off] [--date-style weekday|long|none] [--language ll-CC]
//              [setting=value ...] [p1.l1.t1.weight=300 ...]
// Without --skin it draws the default clock. --locale stands in for the regional format.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>

#include "animelogon/clock.h"
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
    SkinPreview preview;
    if (!preview.Render(background, skin::Resolve(s, values), style, StandardFormat(locale), now, &out)) {
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
