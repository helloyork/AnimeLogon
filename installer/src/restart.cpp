#include "restart.h"

#include <windows.h>

#include <cwctype>

#include "animelogon/log.h"

namespace restart {

namespace {

constexpr const wchar_t *kSessionManager = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
constexpr const wchar_t *kValues[] = {L"PendingFileRenameOperations", L"PendingFileRenameOperations2"};

bool SameText(const std::wstring &a, size_t from, size_t count, const std::wstring &b) {
    return count == b.size() &&
           CompareStringOrdinal(a.c_str() + from, (int)count, b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// The Win32 path in an entry's `from`: without its flags (*1, !) and the \??\ in front.
std::wstring Win32Path(const std::wstring &from) {
    size_t i = 0;
    if (i < from.size() && from[i] == L'*') {
        ++i;
        while (i < from.size() && iswdigit(from[i])) ++i;
    }
    while (i < from.size() && from[i] == L'!') ++i;
    std::wstring path = from.substr(i);
    if (path.rfind(L"\\??\\", 0) == 0) path.erase(0, 4);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

}  // namespace

std::vector<Operation> Parse(const wchar_t *data, size_t chars) {
    std::vector<Operation> ops;
    size_t i = 0;
    auto next = [&]() {
        const size_t start = i;
        while (i < chars && data[i] != L'\0') ++i;
        std::wstring s(data + start, i - start);
        if (i < chars) ++i;  // the string's null
        return s;
    };
    while (i < chars) {
        Operation op;
        op.from = next();
        if (op.from.empty()) break;  // the list's final empty string: no entry has an empty `from`
        op.to = next();
        ops.push_back(std::move(op));
    }
    return ops;
}

std::wstring Serialize(const std::vector<Operation> &operations) {
    std::wstring data;
    for (const Operation &op : operations) {
        data += op.from;
        data += L'\0';
        data += op.to;
        data += L'\0';
    }
    data += L'\0';
    return data;
}

bool DeletesInstalled(const Operation &op, const std::wstring &dir) {
    if (!op.to.empty() || dir.empty()) return false;
    const std::wstring path = Win32Path(op.from);
    const bool inside = SameText(path, 0, path.size(), dir) ||
                        (path.size() > dir.size() && path[dir.size()] == L'\\' && SameText(path, 0, dir.size(), dir));
    if (!inside) return false;
    const std::wstring old = L".old";
    return !(path.size() > old.size() && SameText(path, path.size() - old.size(), old.size(), old));
}

int KeepInstalled(const std::wstring &dir) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSessionManager, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return 0;
    int taken = 0;
    bool failed = false;
    for (const wchar_t *name : kValues) {
        DWORD type = 0, bytes = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_MULTI_SZ || !bytes)
            continue;
        std::wstring data(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &bytes) != ERROR_SUCCESS)
            continue;
        std::vector<Operation> ops = Parse(data.c_str(), bytes / sizeof(wchar_t));
        std::vector<Operation> kept;
        for (Operation &op : ops) {
            if (DeletesInstalled(op, dir)) {
                ALOG(L"install: %s is no longer deleted at the next restart", op.from.c_str());
                ++taken;
            } else {
                kept.push_back(std::move(op));
            }
        }
        if (kept.size() == ops.size()) continue;
        LSTATUS s;
        if (kept.empty()) {
            s = RegDeleteValueW(key, name);
        } else {
            const std::wstring out = Serialize(kept);
            s = RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(out.c_str()),
                               (DWORD)(out.size() * sizeof(wchar_t)));
        }
        if (s != ERROR_SUCCESS) {
            ALOG(L"install: %s could not be written (%ld)", name, (long)s);
            failed = true;
        }
    }
    RegCloseKey(key);
    return failed ? -1 : taken;
}

}  // namespace restart
