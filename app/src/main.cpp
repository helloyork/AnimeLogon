// animelogon.exe: the settings app. Runs as the user; whatever needs administrator rights runs in
// a short elevated child of itself, with the command line commit.h describes. With --agent it is
// instead the service's agent in the signed-in person's session (agent.h); with --tray, the icon
// in the notification area (tray.h).
//
// The settings app's own arguments, in any order:
//   --sandbox <dir>  a window apart from what is installed, on a copy of the data under <dir> (paths.h)
//   --show <what>    comes up at what the icon asked for (tray.h IsShowTarget)
//   --tray           the icon instead of the window, in the sandbox if one is given

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
#include "tray.h"

using namespace animelogon;

namespace {

struct Arguments {
    bool tray = false;
    bool sandbox = false;
    std::wstring dir, show;
};

// False when the arguments are not the settings app's.
bool Parse(const std::vector<std::wstring> &args, Arguments *out) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring &a = args[i];
        if (a == L"--tray" && !out->tray) {
            out->tray = true;
        } else if (a == L"--sandbox" && !out->sandbox && i + 1 < args.size()) {
            out->sandbox = true;
            out->dir = args[++i];
        } else if (a == L"--show" && out->show.empty() && i + 1 < args.size() && !args[i + 1].empty() && app::IsShowTarget(args[i + 1])) {
            out->show = args[++i];
        } else {
            return false;
        }
    }
    return true;
}

bool IsAppArgument(const std::wstring &a) { return a == L"--sandbox" || a == L"--show" || a == L"--tray"; }

// An empty sandbox starts as a fresh installation would: its stores and default settings.
bool PrepareSandbox(const std::wstring &dir) {
    if (!paths::IsPlainAbsolute(dir) || !paths::CreateDirectories(dir)) {
        MessageBoxW(nullptr, L"--sandbox 参数需要指定本地目录的完整路径。", L"AnimeLogon", MB_OK | MB_ICONWARNING);
        return false;
    }
    paths::UseSandbox(dir);
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
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t *, int show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::vector<std::wstring> args = argv ? std::vector<std::wstring>(argv + 1, argv + argc) : std::vector<std::wstring>();
    if (argv) LocalFree(argv);

    if (args.size() == 1 && args[0] == L"--agent") return app::RunAgent();
    Arguments a;
    if (!args.empty() && IsAppArgument(args[0])) {
        if (!Parse(args, &a)) {
            MessageBoxW(nullptr, L"无法识别的命令行参数。", L"AnimeLogon", MB_OK | MB_ICONWARNING);
            return 2;
        }
    } else if (!args.empty() && !args[0].empty() && args[0][0] == L'-') {
        // The elevated child.
        return commit::Run(args);
    }
    if (a.sandbox && !PrepareSandbox(a.dir)) return 2;
    if (a.tray) return app::RunTray();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    instance::Guard only;
    if (only.Acquire(a.sandbox ? paths::SandboxMutex() : L"Local\\AnimeLogon.App") == instance::Result::AlreadyRunning) {
        if (HWND existing = app::FindAppWindow()) {
            if (!app::AskApp(existing, a.show)) {
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
        }
        CoUninitialize();
        return 0;
    }
    // The window's own log, beside its import directories.
    if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\app.log");

    int code = 1;
    {
        app::Host host;
        if (host.Create(show, a.show)) code = host.Run();
    }
    CoUninitialize();
    return code;
}
