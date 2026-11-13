// The files install.exe carries inside itself, so that the download is one file. The build puts
// each one in as a resource of type PAYLOAD (installer/CMakeLists.txt, payload.rc); the ids here
// match the order there.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace payload {

struct File {
    int id;
    const wchar_t *name;  // relative to the install directory
};

// Everything that is installed, programs first, then the licences that go with them.
const std::vector<File> &Files();

// Whether this program carries `name`: install.exe as built does; uninstall.exe carries nothing.
bool Has(const wchar_t *name);
// The bytes of `name`, or none.
bool Bytes(const wchar_t *name, const void **data, DWORD *size);
// Writes `name` to `path`, replacing what is there. ERROR_FILE_NOT_FOUND when not carried.
DWORD Write(const wchar_t *name, const std::wstring &path);
// What all of it takes on disk, for the space check.
unsigned long long TotalBytes();

}  // namespace payload
