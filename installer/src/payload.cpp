#include "payload.h"

namespace payload {

const std::vector<File> &Files() {
    static const std::vector<File> files = {
        {1, L"overlay.exe"},
        {2, L"launcher.exe"},
        {3, L"animelogon.exe"},
        {4, L"scene.exe"},
        {5, L"WebView2Loader.dll"},
        {6, L"uninstall.exe"},
        {7, L"LICENSE.txt"},
        {8, L"licenses\\WebView2.txt"},
        {9, L"licenses\\ThorVG.txt"},
        {10, L"licenses\\RapidJSON.txt"},
    };
    return files;
}

namespace {

const File *Find(const wchar_t *name) {
    for (const File &f : Files())
        if (_wcsicmp(f.name, name) == 0) return &f;
    return nullptr;
}

}  // namespace

bool Bytes(const wchar_t *name, const void **data, DWORD *size) {
    const File *f = Find(name);
    if (!f) return false;
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(f->id), L"PAYLOAD");
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    *data = g ? LockResource(g) : nullptr;
    *size = r ? SizeofResource(self, r) : 0;
    return *data != nullptr;
}

bool Has(const wchar_t *name) {
    const void *data = nullptr;
    DWORD size = 0;
    return Bytes(name, &data, &size);
}

DWORD Write(const wchar_t *name, const std::wstring &path) {
    const void *data = nullptr;
    DWORD size = 0;
    if (!Bytes(name, &data, &size)) return ERROR_FILE_NOT_FOUND;
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

unsigned long long TotalBytes() {
    unsigned long long total = 0;
    for (const File &f : Files()) {
        const void *data = nullptr;
        DWORD size = 0;
        if (Bytes(f.name, &data, &size)) total += size;
    }
    return total;
}

}  // namespace payload
