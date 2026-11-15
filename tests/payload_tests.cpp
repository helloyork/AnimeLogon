// What install.exe carries and which of it a machine takes: the table the build writes against the
// resources the same build put into install.exe, and the choice of architecture.
#include "check.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "payload.h"

namespace {

bool Contains(const std::vector<USHORT> &v, USHORT m) { return std::find(v.begin(), v.end(), m) != v.end(); }

// The machine a PE image is for, from its COFF header; 0 if it is not one.
USHORT ImageMachine(const void *data, DWORD size) {
    const auto *bytes = static_cast<const BYTE *>(data);
    if (size < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return 0;
    const DWORD at = *reinterpret_cast<const DWORD *>(bytes + 0x3C);
    if ((unsigned long long)at + 6 > size || memcmp(bytes + at, "PE\0\0", 4) != 0) return 0;
    return *reinterpret_cast<const USHORT *>(bytes + at + 4);
}

std::vector<BYTE> ReadFileAll(const std::wstring &path) {
    std::vector<BYTE> bytes;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 30)) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr) || got != bytes.size()) bytes.clear();
    }
    CloseHandle(h);
    return bytes;
}

BOOL CALLBACK CollectId(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR into) {
    if (IS_INTRESOURCE(name)) reinterpret_cast<std::vector<int> *>(into)->push_back((int)(ULONG_PTR)name);
    else reinterpret_cast<std::vector<int> *>(into)->push_back(-1);  // a named one: not the build's
    return TRUE;
}

}  // namespace

TEST(PayloadMachineIsTheOneWindowsRunsOnWhenCarried) {
    constexpr USHORT x64 = IMAGE_FILE_MACHINE_AMD64, arm64 = IMAGE_FILE_MACHINE_ARM64;
    // The release's install.exe: x64, with both sets.
    CHECK(payload::Choose(x64, x64, {x64, arm64}) == x64);
    CHECK(payload::Choose(arm64, x64, {x64, arm64}) == arm64);
    // An x64 build without the ARM64 set, run on ARM64 under emulation: nothing to install.
    CHECK(payload::Choose(arm64, x64, {x64}) == 0);
    // An ARM64 build's own install.exe on ARM64.
    CHECK(payload::Choose(arm64, arm64, {arm64}) == arm64);
    // Any other processor.
    CHECK(payload::Choose(IMAGE_FILE_MACHINE_I386, x64, {x64, arm64}) == 0);
    // A build run from its own folder carries nothing and installs the programs beside it, which
    // are its own architecture's.
    CHECK(payload::Choose(x64, x64, {}) == x64);
    CHECK(payload::Choose(arm64, x64, {}) == 0);
    // This machine is one of the two, and this program is built for one of them.
    CHECK(payload::NativeMachine() == x64 || payload::NativeMachine() == arm64);
    CHECK(payload::MachineName(payload::OwnMachine()) == (payload::OwnMachine() == x64 ? L"x64" : L"ARM64"));
    // The tests carry no payload.
    CHECK(payload::Carried().empty());
}

TEST(PayloadTableNamesEachFileOncePerMachine) {
    const std::vector<payload::File> &files = payload::Files();
    std::vector<int> ids;
    std::vector<USHORT> machines;
    for (const payload::File &f : files) {
        CHECK(std::find(ids.begin(), ids.end(), f.id) == ids.end());
        ids.push_back(f.id);
        if (f.kind == payload::Kind::Program && !Contains(machines, f.machine)) machines.push_back(f.machine);
        if (f.kind == payload::Kind::Document) CHECK(f.machine == 0);
        for (const payload::File &g : files)
            if (&g != &f && g.kind == f.kind && g.machine == f.machine) CHECK(_wcsicmp(g.name, f.name) != 0);
    }
    CHECK(Contains(machines, payload::OwnMachine()));
    // Every machine's set has the same programs, the ones the engine relies on among them.
    const std::vector<const wchar_t *> programs = payload::Names(payload::Kind::Program);
    for (USHORT m : machines)
        for (const wchar_t *name : programs) {
            const bool listed = std::any_of(files.begin(), files.end(), [&](const payload::File &f) {
                return f.kind == payload::Kind::Program && f.machine == m && _wcsicmp(f.name, name) == 0;
            });
            CHECK(listed);
        }
    for (const wchar_t *needed : {L"overlay.exe", L"launcher.exe", L"animelogon.exe", L"scene.exe", L"WebView2Loader.dll", L"uninstall.exe"})
        CHECK(std::any_of(programs.begin(), programs.end(), [&](const wchar_t *n) { return _wcsicmp(n, needed) == 0; }));
    const std::vector<const wchar_t *> documents = payload::Names(payload::Kind::Document);
    for (const wchar_t *needed : {L"LICENSE.txt", L"licenses\\npm.txt", L"licenses\\WebView2.txt", L"licenses\\WebView2-NOTICE.txt"})
        CHECK(std::any_of(documents.begin(), documents.end(), [&](const wchar_t *n) { return _wcsicmp(n, needed) == 0; }));
}

// install.exe as this build made it carries exactly what the table lists, each program for the
// machine the table says, the build's own programs byte for byte; and the licences of the npm
// packages in the settings app.
TEST(InstallExeCarriesWhatTheTableLists) {
    // CMake writes the path with forward slashes, which LoadLibraryEx does not take.
    std::wstring exe = L"" ANIMELOGON_INSTALL_EXE;
    std::replace(exe.begin(), exe.end(), L'/', L'\\');
    HMODULE m = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    CHECK(m != nullptr);
    if (!m) return;
    std::vector<int> carried;
    EnumResourceNamesW(m, L"PAYLOAD", CollectId, reinterpret_cast<LONG_PTR>(&carried));
    std::vector<int> listed;
    for (const payload::File &f : payload::Files()) listed.push_back(f.id);
    std::sort(carried.begin(), carried.end());
    std::sort(listed.begin(), listed.end());
    CHECK(carried == listed);

    const std::wstring bin = exe.substr(0, exe.find_last_of(L"\\/"));
    for (const payload::File &f : payload::Files()) {
        const void *data = nullptr;
        DWORD size = 0;
        CHECK(payload::Bytes(f.name, f.kind == payload::Kind::Program ? f.machine : payload::OwnMachine(), &data, &size, m));
        if (!data) continue;
        if (f.kind == payload::Kind::Program) {
            CHECK(ImageMachine(data, size) == f.machine);
            if (f.machine == payload::OwnMachine()) {
                const std::vector<BYTE> built = ReadFileAll(bin + L"\\" + f.name);
                CHECK(built.size() == size && memcmp(built.data(), data, size) == 0);
            }
        } else if (_wcsicmp(f.name, L"licenses\\npm.txt") == 0) {
            const std::string text(static_cast<const char *>(data), size);
            for (const char *package : {"\r\nreact 19.", "\r\nreact-dom 19.", "\r\n@fluentui/react-icons ", "\r\ntabster "})
                CHECK(text.find(package) != std::string::npos);
            CHECK(text.find("Copyright (c) 2020 Microsoft Corporation") != std::string::npos);
        }
    }
    // Every set it has a program of, it has whole; its own among them.
    const std::vector<USHORT> sets = payload::Carried(m);
    CHECK(Contains(sets, payload::OwnMachine()));
    for (const payload::File &f : payload::Files())
        if (f.kind == payload::Kind::Program) CHECK(Contains(sets, f.machine));
    FreeLibrary(m);
}
