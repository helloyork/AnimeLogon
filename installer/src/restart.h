#pragma once

// The files Windows deletes at the next restart (Session Manager's PendingFileRenameOperations),
// as far as an install needs them. An uninstall that cannot delete everything at once --
// uninstall.exe deleting itself, a program still running -- leaves the rest, and the install
// directory with it, to be deleted at the next restart. Installing again before that restart has to
// take those deletions back, or the restart deletes the files just written.

#include <string>
#include <vector>

namespace restart {

// One entry: `from` renamed to `to` at the next restart, or deleted when `to` is empty. Both as
// Windows keeps them: NT paths (\??\C:\...), sometimes behind a prefix of flags (*1, !).
struct Operation {
    std::wstring from, to;
};

// The entries in the value's REG_MULTI_SZ data, `chars` long: pairs of strings. A deletion's empty
// `to` looks like the list's end, so the length decides where it ends, not an empty string.
std::vector<Operation> Parse(const wchar_t *data, size_t chars);
// The data for `operations`, the list's final empty string included.
std::wstring Serialize(const std::vector<Operation> &operations);

// Whether `op` deletes `dir` or anything in it, other than a copy of an installed file that was in
// use and moved aside to be deleted (*.old: engine.cpp's WriteOver). `dir` is a Win32 path without
// a trailing backslash; the comparison ignores case.
bool DeletesInstalled(const Operation &op, const std::wstring &dir);

// Takes back the deletions at the next restart of `dir` and what is in it (DeletesInstalled),
// keeping every other entry as it is, in both of the values Windows reads. Returns how many were
// taken back; -1 when a value could not be written.
int KeepInstalled(const std::wstring &dir);

}  // namespace restart
