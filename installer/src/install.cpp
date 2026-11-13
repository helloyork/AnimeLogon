// install.exe: installs AnimeLogon, carrying every file it installs (payload.h). Requires
// administrator rights (res/admin.manifest).
//
// Without arguments it shows the wizard (wizards.h). The first page says what installing turns off
// in Windows, unless the Windows lock screen is off already. --quiet asks nothing, shows nothing
// and prints to the parent console; scripted installs tell the person themselves. It is a windows
// program, so a script waits for it with `start /wait` or Start-Process -Wait.
//   --dir <path>   where to install (inside Program Files); an update goes where it was
//   --lang <tag>   zh-Hans, zh-Hant, en or ja: the wizard's language, and the machine's
//   --setup-in-app with --quiet: installs without switching AnimeLogon on. Windows' lock screen
//                  and other programs drawing the logon screen are left as they are, and the
//                  settings app opens on a wizard that says what switching on changes and does it.
//                  An update keeps AnimeLogon on or off as it was. Steam's install script runs this.
// Exit codes: 0 installed, 1 failed, 1223 (ERROR_CANCELLED) cancelled.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "animelogon/i18n.h"
#include "animelogon/log.h"
#include "animelogon/machine.h"
#include "animelogon/others.h"

#include "engine.h"
#include "wizards.h"

namespace {

namespace i18n = animelogon::i18n;

const wchar_t *Arg(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    return nullptr;
}

bool HasArg(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return true;
    return false;
}

void Print(const std::wstring &text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    DWORD wrote = 0;
    const std::wstring line = text + L"\r\n";
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), (DWORD)line.size(), &wrote, nullptr);
}

// --dir as an absolute path without a trailing backslash; the engine decides if it is allowed.
std::wstring FullPath(const wchar_t *path) {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetFullPathNameW(path, (DWORD)buf.size(), buf.data(), nullptr);
    std::wstring full = n && n < buf.size() ? std::wstring(buf.data(), n) : std::wstring(path);
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return full;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool quiet = HasArg(argc, argv, L"--quiet");

    // The language: as asked, or as AnimeLogon is already shown on this machine, or as Windows is.
    i18n::Lang lang = i18n::Preferred();
    const wchar_t *asked = Arg(argc, argv, L"--lang");
    const bool langGiven = asked && i18n::FromTag(asked, &lang);
    i18n::Use(lang);

    // The shortcuts and the folder picker are COM.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // An update goes where the installed copy is; a new install to Program Files unless told.
    std::wstring target = animelogon::machine::InstallDir();
    if (target.empty()) target = engine::DefaultInstallDir();
    if (const wchar_t *dir = Arg(argc, argv, L"--dir")) target = FullPath(dir);

    int code = 0;
    if (quiet) {
        engine::InstallOptions options;
        if (langGiven) options.language = i18n::Tag(lang);
        options.setupInApp = HasArg(argc, argv, L"--setup-in-app");
        const engine::Report report = engine::Install(target, options);
        Print(report.message);
        code = report.ok ? 0 : 1;
    } else {
        wizard::InstallWizard w;
        w.targetDir = target;
        w.installedVersion = engine::InstalledVersion();
        w.askConsent = !animelogon::machine::WindowsLockScreenApplied();
        for (const animelogon::others::Program &p : animelogon::others::Installed())
            if (p.on) w.othersOn += (w.othersOn.empty() ? L"" : i18n::Tr("list.join")) + p.name;
        w.language = i18n::Tag(lang);
        w.SetLocale(i18n::Locale(lang));
        code = w.Run(i18n::Tr("setup.window"));
    }
    if (FAILED(co)) ALOG(L"install: COM unavailable (0x%08lx)", (unsigned long)co);
    if (SUCCEEDED(co)) CoUninitialize();
    LocalFree(argv);
    return code;
}
