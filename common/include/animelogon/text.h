// String helpers shared by every program.
#pragma once

#include <cstdarg>
#include <string>
#include <string_view>

namespace animelogon {

std::string ToUtf8(std::wstring_view text);
std::wstring FromUtf8(std::string_view text);

std::wstring Format(const wchar_t *format, ...);
std::wstring FormatV(const wchar_t *format, va_list args);

// ASCII-only case folding, for keys and fixed tokens.
bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
std::wstring_view Trim(std::wstring_view text);

// Parses a whole decimal integer; rejects trailing text and overflow.
bool ParseInt(std::wstring_view text, long long *value);

// 64-bit FNV-1a, as 16 lowercase hex digits. Stable across builds and architectures.
std::wstring HashKey(std::wstring_view text);

// Random lowercase hex string of `bytes` * 2 digits.
std::wstring RandomHex(size_t bytes);

}  // namespace animelogon
