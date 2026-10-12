// animelogon.exe: the settings app. Runs as the user; whatever needs administrator rights runs in
// a short elevated child of itself, with the command line commit.h describes.

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"

#include "commit.h"
#include "host.h"

using namespace animelogon;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && argv[1][0] == L'-') {
        // The elevated child.
        const std::vector<std::wstring> args(argv + 1, argv + argc);
        LocalFree(argv);
        return commit::Run(args);
    }
    if (argv) LocalFree(argv);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    instance::Guard only;
    if (only.Acquire(L"Local\\AnimeLogon.App") == instance::Result::AlreadyRunning) {
        if (HWND existing = FindWindowW(L"AnimeLogonApp", nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CoUninitialize();
        return 0;
    }
    // The window's own log, beside its import directories.
    if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\app.log");

    int code = 1;
    {
        app::Host host;
        if (host.Create(show)) code = host.Run();
    }
    CoUninitialize();
    return code;
}
