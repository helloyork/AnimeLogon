// Theme packages open in AnimeLogon: double-clicking an .altheme file starts the settings app with
// its path, which imports it (app/src/arguments.h). Registered for every account, as AnimeLogon is
// installed: a ProgID of AnimeLogon's own under HKLM\Software\Classes, and the extension pointing to
// it. A person's own choice (Explorer's UserChoice, under HKCU) is never touched, nor is any other
// extension. Scenes (.alscene) are not registered while the importer turns them away
// (staging::kTakesScenes).
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace assoc {

constexpr const wchar_t *kExtension = L".altheme";
constexpr const wchar_t *kProgId = L"AnimeLogon.Theme";

// One string value under HKLM\Software\Classes: its key below Classes, its name (empty for the
// key's default value) and its data. With `ifFree` it is written only where the value is absent,
// empty or already this data: the extension's default, which another program may have claimed first.
struct Entry {
    std::wstring key, name, data;
    bool ifFree = false;
};

// What installing to `installDir` writes, in order. `typeName` is what Explorer calls such a file
// (its Type column), in the language the installer was shown in.
std::vector<Entry> Entries(const std::wstring &installDir, const std::wstring &typeName);

// HKLM\Software\Classes as Register and Unregister need it; the tests use one in memory.
class Classes {
public:
    virtual ~Classes() = default;
    // False when the value is not there. A value that is not a string reads as "?".
    virtual bool Get(const std::wstring &key, const std::wstring &name, std::wstring *data) = 0;
    // Creates the key, and those above it, as needed.
    virtual DWORD Set(const std::wstring &key, const std::wstring &name, const std::wstring &data) = 0;
    // The deletions succeed where there is nothing to delete.
    virtual DWORD DeleteValue(const std::wstring &key, const std::wstring &name) = 0;
    virtual DWORD DeleteTree(const std::wstring &key) = 0;
    // Deletes `key` only if it holds neither values nor subkeys.
    virtual DWORD DeleteIfEmpty(const std::wstring &key) = 0;
};

// The machine's, in the 64-bit view of the registry.
std::unique_ptr<Classes> MachineClasses();

// Writes Entries. The ProgID is written afresh, so that an update leaves exactly what this version
// writes, wherever it is installed. Returns the first failure, having written all it could.
DWORD Register(Classes &classes, const std::wstring &installDir, const std::wstring &typeName);
// Takes away what Register wrote: the ProgID, AnimeLogon among the programs the extension's Open
// with offers, and the extension's default while it is still the ProgID; then the extension's key,
// and its OpenWithProgids, if that leaves them empty. What another program put there stays.
DWORD Unregister(Classes &classes);
// Whether the extension's default is AnimeLogon's ProgID, which is what a double click opens when
// nobody has chosen otherwise.
bool IsDefault(Classes &classes);

// Tells Explorer the associations changed, so that icons and Open with follow without a sign-out.
void Notify();

}  // namespace assoc
