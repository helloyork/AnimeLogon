// Line logs, capped at 20,000 lines with one previous generation kept as <name>.1.
#pragma once

#include <string>

namespace animelogon::log {

// Selects this process's log file. Lines written before Open go to the debugger only, and
// so do a privileged process's lines while the file's directory is not trusted.
void Open(const std::wstring &path);

void Write(const wchar_t *format, ...);

// Like Write, but never waits for another thread's write. For watchdogs.
bool TryWrite(const wchar_t *format, ...);

}  // namespace animelogon::log

#define ALOG(...) ::animelogon::log::Write(__VA_ARGS__)
