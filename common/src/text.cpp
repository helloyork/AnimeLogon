#include "animelogon/text.h"

#include <windows.h>
#include <bcrypt.h>

#include <climits>
#include <cwchar>

namespace animelogon {

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr,
                            nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}

std::wstring FormatV(const wchar_t *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int n = _vscwprintf(format, copy);
    va_end(copy);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    vswprintf_s(out.data(), out.size() + 1, format, args);
    return out;
}

std::wstring Format(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    std::wstring out = FormatV(format, args);
    va_end(args);
    return out;
}

static wchar_t FoldAscii(wchar_t c) { return (c >= L'A' && c <= L'Z') ? c + 32 : c; }

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (FoldAscii(a[i]) != FoldAscii(b[i])) return false;
    return true;
}

std::wstring_view Trim(std::wstring_view text) {
    const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

bool ParseInt(std::wstring_view text, long long *value) {
    text = Trim(text);
    if (text.empty()) return false;
    bool negative = false;
    size_t i = 0;
    if (text[0] == L'-' || text[0] == L'+') {
        negative = text[0] == L'-';
        i = 1;
    }
    if (i == text.size()) return false;
    unsigned long long v = 0;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
        if (v > (unsigned long long)LLONG_MAX) return false;
    }
    *value = negative ? -(long long)v : (long long)v;
    return true;
}

std::wstring HashKey(std::wstring_view text) {
    unsigned long long h = 14695981039346656037ull;
    for (wchar_t c : text) {
        const unsigned short u = static_cast<unsigned short>(c);
        h = (h ^ (u & 0xFF)) * 1099511628211ull;
        h = (h ^ (u >> 8)) * 1099511628211ull;
    }
    wchar_t buf[17];
    swprintf_s(buf, L"%016llx", h);
    return buf;
}

std::wstring RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(raw.data()), (ULONG)raw.size(),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes * 2);
    for (unsigned char b : raw) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace animelogon
