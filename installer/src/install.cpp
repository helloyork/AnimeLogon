// install.exe: installs AnimeLogon. Requires administrator rights (res/admin.manifest).
// Before changing anything it asks whether to go on, since installing turns the Windows lock
// screen off; a "No" exits with ERROR_CANCELLED and the machine untouched. With --quiet it asks
// nothing and prints to the parent console instead of showing message boxes: scripted installs
// tell the person themselves.

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "animelogon/log.h"
#include "animelogon/machine.h"

#include "engine.h"

namespace {

// What installing takes away from Windows, as the README's "Changes to your system" puts it.
// Not asked when the Windows lock screen is off already (an update, or a machine where it was
// turned off before AnimeLogon): then nothing goes that was there.
bool Agreed(bool quiet) {
    if (quiet || animelogon::machine::WindowsLockScreenApplied()) return true;
    const wchar_t *text =
        L"AnimeLogon 会关闭 Windows 锁屏，换成自己绘制的锁屏。\n\n"
        L"在此期间，Windows 聚焦和锁屏小组件不可用，也无法在 Windows 设置中更改锁屏图片。\n\n"
        L"在「AnimeLogon 设置 > 设置」中关闭「启用 AnimeLogon」，或卸载 AnimeLogon，即可恢复。\n\n"
        L"是否继续安装？";
    return MessageBoxW(nullptr, text, L"安装 AnimeLogon", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1) == IDYES;
}

bool HasArg(int argc, wchar_t **argv, const wchar_t *name) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) return true;
    return false;
}

void Say(bool quiet, const std::wstring &text, bool error) {
    if (quiet) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD wrote = 0;
            const std::wstring line = text + L"\r\n";
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), (DWORD)line.size(), &wrote, nullptr);
        }
    } else {
        MessageBoxW(nullptr, text.c_str(), L"AnimeLogon", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
    }
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

int wmain(int argc, wchar_t **argv) {
    const bool quiet = HasArg(argc, argv, L"--quiet");
    if (!Agreed(quiet)) return ERROR_CANCELLED;
    // The Start-menu shortcut is made through COM.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    std::wstring target = engine::DefaultInstallDir();
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--dir") == 0 && i + 1 < argc) target = FullPath(argv[++i]);

    const engine::Report report = engine::Install(target);
    if (FAILED(co)) ALOG(L"install: COM unavailable (0x%08lx)", (unsigned long)co);
    Say(quiet, report.message, !report.ok);
    if (SUCCEEDED(co)) CoUninitialize();
    return report.ok ? 0 : 1;
}
