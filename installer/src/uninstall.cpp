// uninstall.exe: removes AnimeLogon and restores what it changed. Requires administrator rights
// (res/admin.manifest). Without arguments it asks first, with the choice of keeping the themes
// (wizards.h); --quiet asks nothing and prints to the parent console, and --purge then also deletes
// the themes, wallpapers and settings. Exit codes: 0 removed, 1 something could not be undone,
// 1223 (ERROR_CANCELLED) cancelled.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "animelogon/i18n.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"

#include "engine.h"
#include "wizards.h"

namespace {

bool HasArg(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return true;
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    namespace i18n = animelogon::i18n;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    animelogon::log::Open(animelogon::paths::LogPath(L"uninstall.log"));
    const bool quiet = HasArg(argc, argv, L"--quiet");
    const i18n::Lang lang = i18n::Preferred();
    i18n::Use(lang);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int code = 0;
    if (quiet) {
        const engine::Report report = engine::Uninstall(!HasArg(argc, argv, L"--purge"));
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD wrote = 0;
            const std::wstring line = report.message + L"\r\n";
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), (DWORD)line.size(), &wrote, nullptr);
        }
        code = report.ok ? 0 : 1;
    } else {
        wizard::UninstallWizard w;
        w.SetLocale(i18n::Locale(lang));
        code = w.Run(i18n::Tr("uninstall.window"));
    }
    if (SUCCEEDED(co)) CoUninitialize();
    LocalFree(argv);
    return code;
}
