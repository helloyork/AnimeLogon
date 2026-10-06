// uninstall.exe: removes AnimeLogon and restores what it changed. Requires administrator
// rights (res/admin.manifest). --purge also deletes imported videos and settings.

#include <windows.h>

#include <string>

#include "animelogon/log.h"
#include "animelogon/paths.h"

#include "engine.h"

namespace {

bool HasArg(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return true;
    return false;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    animelogon::log::Open(animelogon::paths::LogPath(L"uninstall.log"));
    const bool quiet = HasArg(argc, argv, L"--quiet");
    const bool purge = HasArg(argc, argv, L"--purge");

    const engine::Report report = engine::Uninstall(!purge);
    if (quiet) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD wrote = 0;
            const std::wstring line = report.message + L"\r\n";
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), (DWORD)line.size(), &wrote, nullptr);
        }
    } else {
        MessageBoxW(nullptr, report.message.c_str(), L"AnimeLogon",
                    MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    return report.ok ? 0 : 1;
}
