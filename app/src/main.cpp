// animelogon.exe: the settings app. Runs as the user; whatever needs administrator rights runs in
// a short elevated child of itself, with the command line commit.h describes. With --agent it is
// instead the service's agent in the signed-in person's session (agent.h).

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"

#include "agent.h"
#include "commit.h"
#include "host.h"

using namespace animelogon;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 2 && std::wstring(argv[1]) == L"--agent") {
        LocalFree(argv);
        return app::RunAgent();
    }
    // A window apart from what is installed, on a copy of the data under <dir> (paths.h).
    const bool sandbox = argv && argc == 3 && std::wstring(argv[1]) == L"--sandbox";
    if (sandbox) {
        if (!paths::IsPlainAbsolute(argv[2]) || !paths::CreateDirectories(argv[2])) {
            MessageBoxW(nullptr, L"--sandbox 参数需要指定本地目录的完整路径。", L"AnimeLogon", MB_OK | MB_ICONWARNING);
            LocalFree(argv);
            return 2;
        }
        paths::UseSandbox(argv[2]);
        // An empty sandbox starts as a fresh installation would: its stores and default settings.
        for (const wchar_t *store : {L"\\themes", L"\\wallpapers", L"\\components"}) paths::CreateDirectories(paths::DataDir() + store);
        if (GetFileAttributesW(paths::SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
            const std::string text = ToUtf8(SerializeSettings(Settings{}));
            const HANDLE h = CreateFileW(paths::SettingsPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wrote = 0;
            if (h != INVALID_HANDLE_VALUE) {
                WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
                CloseHandle(h);
            }
        }
    } else if (argv && argc >= 2 && argv[1][0] == L'-') {
        // The elevated child.
        const std::vector<std::wstring> args(argv + 1, argv + argc);
        LocalFree(argv);
        return commit::Run(args);
    }
    if (argv) LocalFree(argv);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    instance::Guard only;
    if (only.Acquire(sandbox ? paths::SandboxMutex() : L"Local\\AnimeLogon.App") == instance::Result::AlreadyRunning) {
        if (HWND existing = FindWindowW(L"AnimeLogonApp", sandbox ? paths::SandboxTitle().c_str() : nullptr)) {
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
