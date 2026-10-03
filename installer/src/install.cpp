// install.exe: installs AnimeLogon. Requires administrator rights (res/admin.manifest).
// With --quiet it prints to the parent console; otherwise it shows a message box.

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "animelogon/log.h"

#include "engine.h"

namespace {

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
