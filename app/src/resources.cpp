#include "resources.h"

#include <windows.h>

namespace app {

namespace {

struct File {
    const wchar_t *path;
    int id;  // as numbered in web.rc.in
    const wchar_t *type;
};

const File kFiles[] = {
    {L"index.html", 101, L"text/html; charset=utf-8"},
    {L"app.css", 102, L"text/css; charset=utf-8"},
    {L"app.js", 103, L"text/javascript; charset=utf-8"},
};

}  // namespace

bool WebFile(const std::wstring &path, std::vector<uint8_t> *bytes, std::wstring *type) {
    const std::wstring wanted = path.empty() ? L"index.html" : path;
    for (const File &f : kFiles) {
        if (wanted != f.path) continue;
        HMODULE self = GetModuleHandleW(nullptr);
        HRSRC found = FindResourceW(self, MAKEINTRESOURCEW(f.id), RT_RCDATA);
        HGLOBAL loaded = found ? LoadResource(self, found) : nullptr;
        const auto *data = loaded ? static_cast<const uint8_t *>(LockResource(loaded)) : nullptr;
        if (!data) return false;
        bytes->assign(data, data + SizeofResource(self, found));
        *type = f.type;
        return true;
    }
    return false;
}

}  // namespace app
