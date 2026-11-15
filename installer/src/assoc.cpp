#include "assoc.h"

#include <shlobj.h>

namespace assoc {

namespace {

const std::wstring kRoot = L"Software\\Classes";

std::wstring Path(const std::wstring &key) { return kRoot + L"\\" + key; }

std::wstring OpenWith() { return std::wstring(kExtension) + L"\\OpenWithProgids"; }

bool Same(const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

class Machine final : public Classes {
public:
    bool Get(const std::wstring &key, const std::wstring &name, std::wstring *data) override {
        const DWORD flags = RRF_RT_ANY | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY;
        DWORD type = 0, size = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Path(key).c_str(), name.c_str(), flags, &type, nullptr, &size) != ERROR_SUCCESS)
            return false;
        std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
        size = (DWORD)(text.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Path(key).c_str(), name.c_str(), flags, &type, text.data(), &size) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ)) {
            *data = L"?";
            return true;
        }
        text.resize(wcsnlen(text.c_str(), text.size()));
        *data = text;
        return true;
    }

    DWORD Set(const std::wstring &key, const std::wstring &name, const std::wstring &data) override {
        HKEY k = nullptr;
        DWORD e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr, &k, nullptr);
        if (e != ERROR_SUCCESS) return e;
        e = RegSetValueExW(k, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                           (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        return e;
    }

    DWORD DeleteValue(const std::wstring &key, const std::wstring &name) override {
        HKEY k = nullptr;
        DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path(key).c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
        if (e == ERROR_SUCCESS) {
            e = RegDeleteValueW(k, name.c_str());
            RegCloseKey(k);
        }
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
    }

    DWORD DeleteTree(const std::wstring &key) override {
        HKEY root = nullptr;
        DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRoot.c_str(), 0,
                                DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &root);
        if (e == ERROR_SUCCESS) {
            e = RegDeleteTreeW(root, key.c_str());
            RegCloseKey(root);
        }
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
    }

    DWORD DeleteIfEmpty(const std::wstring &key) override {
        HKEY k = nullptr;
        DWORD e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path(key).c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k);
        if (e == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (e != ERROR_SUCCESS) return e;
        DWORD subkeys = 0, values = 0;
        e = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        if (e != ERROR_SUCCESS || subkeys || values) return e;
        e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, Path(key).c_str(), KEY_WOW64_64KEY, 0);
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
    }
};

}  // namespace

std::vector<Entry> Entries(const std::wstring &installDir, const std::wstring &typeName) {
    const std::wstring app = L"\"" + installDir + L"\\animelogon.exe\"";
    const std::wstring progId = kProgId;
    return {
        {progId, L"", typeName},
        {progId + L"\\DefaultIcon", L"", app + L",0"},
        // The settings app imports the package it is given, in the window already open if there is one.
        {progId + L"\\shell\\open\\command", L"", app + L" \"%1\""},
        {kExtension, L"", progId, true},
        // Offered under Open with even where another program is the default.
        {OpenWith(), progId, L""},
    };
}

std::unique_ptr<Classes> MachineClasses() { return std::make_unique<Machine>(); }

DWORD Register(Classes &classes, const std::wstring &installDir, const std::wstring &typeName) {
    DWORD first = ERROR_SUCCESS;
    auto note = [&](DWORD e) {
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    };
    note(classes.DeleteTree(kProgId));
    for (const Entry &entry : Entries(installDir, typeName)) {
        std::wstring have;
        if (entry.ifFree && classes.Get(entry.key, entry.name, &have) && !have.empty() && !Same(have, entry.data)) continue;
        note(classes.Set(entry.key, entry.name, entry.data));
    }
    return first;
}

DWORD Unregister(Classes &classes) {
    DWORD first = ERROR_SUCCESS;
    auto note = [&](DWORD e) {
        if (e != ERROR_SUCCESS && first == ERROR_SUCCESS) first = e;
    };
    note(classes.DeleteTree(kProgId));
    note(classes.DeleteValue(OpenWith(), kProgId));
    note(classes.DeleteIfEmpty(OpenWith()));
    if (IsDefault(classes)) note(classes.DeleteValue(kExtension, L""));
    note(classes.DeleteIfEmpty(kExtension));
    return first;
}

bool IsDefault(Classes &classes) {
    std::wstring have;
    return classes.Get(kExtension, L"", &have) && Same(have, kProgId);
}

void Notify() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

}  // namespace assoc
