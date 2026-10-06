// Files that only administrators can change, and the check the logon screen makes before
// it reads one. Everything the SYSTEM overlay reads from disk goes through IsTrusted.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace animelogon::secure {

// Administrators and SYSTEM: full control. Users: read. Owner: Administrators. Protected.
constexpr const wchar_t *kDirSddl = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
constexpr const wchar_t *kFileSddl = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";

// True when the owner is SYSTEM, Administrators or TrustedInstaller, no other account may
// write the file or replace it, and none of it is a reparse point. For anything inside
// paths::DataDir(), every directory up to and including DataDir is checked too.
bool IsTrusted(const std::wstring &file, std::wstring *why);
bool IsTrustedDirectory(const std::wstring &dir, std::wstring *why);

// A plain file (no link, a single name) owned by SYSTEM, Administrators or TrustedInstaller,
// whatever its DACL. For settings.ini, which Users may rewrite but never create.
bool IsAdminOwnedFile(const std::wstring &file, std::wstring *why);

// Elevated callers only. Creates `dir` (and parents) or takes it back, then applies kDirSddl.
// A directory someone else could change is emptied first, without following links.
DWORD SecureDirectory(const std::wstring &dir);

// Adds a Users read+write ACE to an existing file, so the unelevated settings app can edit
// it. For settings.ini only, which holds ids and enums the SYSTEM side never treats as a
// path, so IsTrusted is deliberately not asked of it.
DWORD GrantUsersWrite(const std::wstring &file);

// Writes `bytes` to `file` through a new administrators-only file in the same directory,
// then renames it over `file`.
DWORD WriteBytes(const std::wstring &file, const void *bytes, size_t size);

// Opens an existing file for reading and keeps writers out until it is closed. Refuses
// reparse points, directories, files with more than one name, and a path that resolves
// anywhere else (a link in one of its directories). `path` must be plain absolute.
HANDLE OpenPlainFile(const std::wstring &path, std::wstring *why);

// Copies the rest of `source`, from its current position, into `file` the same way as
// WriteBytes. `progress` receives bytes copied so far and may return false to cancel.
DWORD CopyInto(HANDLE source, const std::wstring &file,
               const std::function<bool(uint64_t done, uint64_t total)> &progress = {});

// Deletes `dir` and everything under it without following links.
DWORD RemoveTree(const std::wstring &dir);

// Reads a whole file, refusing reparse points and anything larger than `limit`.
bool ReadFileBytes(const std::wstring &file, std::vector<uint8_t> *bytes, size_t limit);
// The same from an open handle, from its current position to the end.
bool ReadHandleBytes(HANDLE h, std::vector<uint8_t> *bytes, size_t limit);

}  // namespace animelogon::secure
