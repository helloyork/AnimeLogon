// animelogon.exe: the settings app. Runs as the user; whatever needs administrator rights runs in
// a short elevated child of itself, with the command line commit.h describes. With --agent it is
// instead the service's agent in the signed-in person's session (agent.h); with --tray, the icon
// in the notification area (tray.h). The settings app's own arguments are in arguments.h.

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "animelogon/i18n.h"
#include "animelogon/instance.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"

#include "agent.h"
#include "arguments.h"
#include "commit.h"
#include "host.h"
#include "lockscreen.h"
#include "tray.h"

using namespace animelogon;

namespace {

// A file given on the command line, by its full path: the window that imports it may have been
// started from another folder.
std::wstring FullPath(const std::wstring &path) {
    const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n) return path;
    std::wstring full(n, L'\0');
    const DWORD wrote = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
    if (!wrote || wrote >= n) return path;
    full.resize(wrote);
    return full;
}

// An empty sandbox starts as a fresh installation would: its stores and default settings.
bool PrepareSandbox(const std::wstring &dir) {
    if (!paths::IsPlainAbsolute(dir) || !paths::CreateDirectories(dir)) {
        i18n::Use(i18n::Preferred());
        MessageBoxW(nullptr, i18n::Tr("app.error.sandbox_arg").c_str(), L"AnimeLogon", MB_OK | MB_ICONWARNING);
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

    app::Arguments a;
    switch (app::ReadArguments(args, &a)) {
    case app::Role::Agent:
        return app::RunAgent();
    case app::Role::RestoreLockScreen:
        // Started by the service in the person's own session, as AnimeLogon is switched off or removed.
        if (paths::CreateDirectories(paths::UserDataDir())) log::Open(paths::UserDataDir() + L"\\agent.log");
        return app::lockscreen::Restore() ? 0 : 1;
    case app::Role::Helper:
        return commit::Run(args);
    case app::Role::Unrecognized:
        i18n::Use(i18n::Preferred());
        MessageBoxW(nullptr, i18n::Tr("app.error.args").c_str(), L"AnimeLogon", MB_OK | MB_ICONWARNING);
        return 2;
    case app::Role::App:
        break;
    }
    if (a.sandbox && !PrepareSandbox(a.dir)) return 2;
    // The language of the window or the icon and of everything they say: the person's choice, the
    // machine's, or Windows'. After the sandbox is known, which keeps a choice of its own.
    i18n::Use(i18n::Preferred());
    if (a.tray) return app::RunTray();
    if (!a.open.empty()) a.open = FullPath(a.open);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    instance::Guard only;
    if (only.Acquire(a.sandbox ? paths::SandboxMutex() : L"Local\\AnimeLogon.App") == instance::Result::AlreadyRunning) {
        // The window already open shows what was asked, or imports the file, instead of a second one.
        // Started a moment before (several files opened at once), it may not have its window yet.
        HWND existing = app::FindAppWindow();
        for (int tries = 0; !existing && !a.open.empty() && tries < 50; ++tries) {
            Sleep(100);
            existing = app::FindAppWindow();
        }
        if (existing) {
            const bool asked = a.open.empty() ? app::AskApp(existing, a.show) : app::AskAppToOpen(existing, a.open);
            if (!asked) {
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
        if (host.Create(show, a.show, a.open)) code = host.Run();
    }
    CoUninitialize();
    return code;
}
