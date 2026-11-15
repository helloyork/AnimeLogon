#include "payload.h"

#include <algorithm>

namespace payload {

const std::vector<File> &Files() {
    static const std::vector<File> files = {
#include "payload_files.inc"
    };
    return files;
}

std::vector<const wchar_t *> Names(Kind kind) {
    std::vector<const wchar_t *> names;
    for (const File &f : Files()) {
        if (f.kind != kind) continue;
        const bool seen = std::any_of(names.begin(), names.end(), [&](const wchar_t *n) { return _wcsicmp(n, f.name) == 0; });
        if (!seen) names.push_back(f.name);
    }
    return names;
}

USHORT OwnMachine() {
#if defined(_M_ARM64)
    return IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64)
    return IMAGE_FILE_MACHINE_AMD64;
#else
#error "AnimeLogon is built for x64 and ARM64 only."
#endif
}

USHORT NativeMachine() {
    // IsWow64Process2 reports the machine Windows itself runs on, also to an x64 program that
    // Windows 11 on ARM runs under emulation: such a process is not a WOW64 one, so its own machine
    // comes back IMAGE_FILE_MACHINE_UNKNOWN and the native one IMAGE_FILE_MACHINE_ARM64.
    // GetNativeSystemInfo would not do: under x64 emulation it reports an x64 processor.
    // (GetMachineTypeAttributes, which says whether a machine type can run at all, exists only from
    // Windows 11; this is needed on Windows 10 too.) Looked up rather than imported, since Windows 10
    // before version 1709 does not have it; no such Windows runs an x64 program on ARM, so there the
    // machine is this program's own.
    using Query = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT process = IMAGE_FILE_MACHINE_UNKNOWN, native = IMAGE_FILE_MACHINE_UNKNOWN;
    if (query && query(GetCurrentProcess(), &process, &native) && native != IMAGE_FILE_MACHINE_UNKNOWN) return native;
    return OwnMachine();
}

std::wstring MachineName(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case IMAGE_FILE_MACHINE_I386: return L"x86";
    default: {
        wchar_t text[16];
        swprintf_s(text, L"0x%04X", machine);
        return text;
    }
    }
}

namespace {

const File *Find(const wchar_t *name, USHORT machine) {
    for (const File &f : Files())
        if (_wcsicmp(f.name, name) == 0 && (f.kind != Kind::Program || f.machine == machine)) return &f;
    return nullptr;
}

bool Resource(HMODULE module, int id, const void **data, DWORD *size) {
    if (!module) module = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(module, MAKEINTRESOURCEW(id), L"PAYLOAD");
    HGLOBAL g = r ? LoadResource(module, r) : nullptr;
    *data = g ? LockResource(g) : nullptr;
    *size = r ? SizeofResource(module, r) : 0;
    return *data != nullptr;
}

}  // namespace

std::vector<USHORT> Carried(HMODULE module) {
    std::vector<USHORT> machines, incomplete;
    for (const File &f : Files()) {
        if (f.kind != Kind::Program) continue;
        const void *data = nullptr;
        DWORD size = 0;
        const bool there = Resource(module, f.id, &data, &size);
        auto &into = there ? machines : incomplete;
        if (std::find(into.begin(), into.end(), f.machine) == into.end()) into.push_back(f.machine);
    }
    // A set with a program missing is no set: those programs are not installed anywhere.
    machines.erase(std::remove_if(machines.begin(), machines.end(),
                                  [&](USHORT m) { return std::find(incomplete.begin(), incomplete.end(), m) != incomplete.end(); }),
                   machines.end());
    return machines;
}

USHORT Choose(USHORT native, USHORT own, const std::vector<USHORT> &carried) {
    if (carried.empty()) return native == own ? own : 0;
    return std::find(carried.begin(), carried.end(), native) != carried.end() ? native : 0;
}

USHORT InstallMachine() {
    static const USHORT machine = Choose(NativeMachine(), OwnMachine(), Carried());
    return machine;
}

bool Bytes(const wchar_t *name, USHORT machine, const void **data, DWORD *size, HMODULE module) {
    *data = nullptr;
    *size = 0;
    const File *f = Find(name, machine);
    return f && Resource(module, f->id, data, size);
}

bool Has(const wchar_t *name, USHORT machine) {
    const void *data = nullptr;
    DWORD size = 0;
    return Bytes(name, machine, &data, &size);
}

DWORD Write(const wchar_t *name, USHORT machine, const std::wstring &path) {
    const void *data = nullptr;
    DWORD size = 0;
    if (!Bytes(name, machine, &data, &size)) return ERROR_FILE_NOT_FOUND;
    // Written fresh, so nothing comes along that a copy would carry over (the zone a download
    // was marked with, an old file's ACL).
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, data, size, &wrote, nullptr) && wrote == size;
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (e != ERROR_SUCCESS) DeleteFileW(path.c_str());
    return e;
}

unsigned long long TotalBytes(USHORT machine) {
    unsigned long long total = 0;
    for (const File &f : Files()) {
        if (f.kind == Kind::Program && f.machine != machine) continue;
        const void *data = nullptr;
        DWORD size = 0;
        if (Resource(nullptr, f.id, &data, &size)) total += size;
    }
    return total;
}

}  // namespace payload
