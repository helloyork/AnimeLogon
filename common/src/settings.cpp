#include "animelogon/settings.h"

#include <cstdint>
#include <cwchar>

#include <windows.h>

#include "animelogon/library.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"

namespace animelogon {
namespace {

constexpr size_t kMaxSettingsBytes = 256 * 1024;
constexpr int kReadAttempts = 5;
constexpr DWORD kRetryMs = 50;
constexpr const wchar_t *kScreenPrefix = L"screen.";
constexpr const wchar_t *kSkinPrefix = L"skin.";

bool ParseBool(std::wstring_view v, bool *out) {
    if (EqualsNoCase(v, L"true") || v == L"1") return *out = true, true;
    if (EqualsNoCase(v, L"false") || v == L"0") return *out = false, true;
    return false;
}

bool ParseVolume(std::wstring_view v, int *out) {
    long long n = 0;
    if (!ParseInt(v, &n) || n < 0 || n > 100) return false;
    *out = (int)n;
    return true;
}

bool IsValueText(std::wstring_view value) {
    if (value.size() > 128) return false;
    for (wchar_t c : value)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool IsMonitorKey(std::wstring_view key) {
    if (key.size() != 16) return false;
    for (wchar_t c : key)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

}  // namespace

const wchar_t *ToString(MonitorMode mode) {
    switch (mode) {
    case MonitorMode::Span: return L"span";
    case MonitorMode::PerMonitor: return L"per-monitor";
    default: return L"duplicate";
    }
}

const wchar_t *ToString(Scaling scaling) {
    switch (scaling) {
    case Scaling::Fit: return L"fit";
    case Scaling::Stretch: return L"stretch";
    default: return L"fill";
    }
}

std::wstring Settings::VideoFor(const std::wstring &monitorKey) const {
    if (monitorMode == MonitorMode::PerMonitor) {
        const auto it = screens.find(monitorKey);
        if (it != screens.end()) return it->second;
    }
    return video;
}

Settings ParseSettings(const std::wstring &text, std::vector<std::wstring> *problems) {
    Settings s;
    size_t lineNo = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring_view line = Trim(std::wstring_view(text).substr(start, end - start));
        start = end + 1;
        ++lineNo;
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring_view::npos) {
            if (problems) problems->push_back(Format(L"line %zu: no '='", lineNo));
            continue;
        }
        const std::wstring_view key = Trim(line.substr(0, eq));
        const std::wstring_view value = Trim(line.substr(eq + 1));
        bool ok = true;
        if (key == L"monitor_mode") {
            if (value == L"duplicate") s.monitorMode = MonitorMode::Duplicate;
            else if (value == L"span") s.monitorMode = MonitorMode::Span;
            else if (value == L"per-monitor") s.monitorMode = MonitorMode::PerMonitor;
            else ok = false;
        } else if (key == L"scaling") {
            if (value == L"fill") s.scaling = Scaling::Fill;
            else if (value == L"fit") s.scaling = Scaling::Fit;
            else if (value == L"stretch") s.scaling = Scaling::Stretch;
            else ok = false;
        } else if (key == L"video") {
            ok = value.empty() || IsVideoId(value);
            if (ok) s.video = value;
        } else if (key.substr(0, wcslen(kScreenPrefix)) == kScreenPrefix) {
            const std::wstring_view monitor = key.substr(wcslen(kScreenPrefix));
            ok = IsMonitorKey(monitor) && (value.empty() || IsVideoId(value));
            if (ok && !value.empty()) s.screens[std::wstring(monitor)] = value;
        } else if (key == L"audio") {
            ok = ParseBool(value, &s.audio.enabled);
        } else if (key == L"audio_device") {
            ok = value.size() < 512;
            if (ok) s.audio.device = value == L"auto" ? std::wstring() : std::wstring(value);
        } else if (key == L"audio_volume") {
            ok = ParseVolume(value, &s.audio.volume);
        } else if (key == L"video_audio") {
            ok = ParseBool(value, &s.audio.videoTrack.enabled);
        } else if (key == L"video_audio_volume") {
            ok = ParseVolume(value, &s.audio.videoTrack.volume);
        } else if (key == L"clock") {
            ok = ParseBool(value, &s.clock.enabled);
        } else if (key == L"clock_displays") {
            ok = Parse(std::wstring(value), &s.clock.displays);
        } else if (key == L"clock_hours") {
            ok = Parse(std::wstring(value), &s.clock.style.hours);
        } else if (key == L"clock_ampm") {
            ok = ParseBool(value, &s.clock.style.ampm);
        } else if (key == L"clock_date") {
            ok = Parse(std::wstring(value), &s.clock.style.date);
        } else if (key == L"clock_language") {
            ok = value.empty() || IsClockLanguage(std::wstring(value));
            if (ok) s.clock.style.locale = value;
        } else if (key == L"skin") {
            ok = skin::IsSkinId(std::wstring(value));
            if (ok) s.clock.skin = value;
        } else if (key.substr(0, wcslen(kSkinPrefix)) == kSkinPrefix) {
            // skin.<skin id>.<setting id>, or skin.<skin id>.<element>.<attribute> for one attribute
            // adjusted by hand: checked against the skin itself when it is drawn.
            const std::wstring_view rest = key.substr(wcslen(kSkinPrefix));
            const size_t dot = rest.find(L'.');
            const std::wstring name = dot == std::wstring_view::npos ? L"" : std::wstring(rest.substr(dot + 1));
            ok = dot != std::wstring_view::npos && skin::IsSkinId(std::wstring(rest.substr(0, dot))) &&
                 (skin::IsSettingId(name) || skin::IsAdjustmentKey(name)) && IsValueText(value);
            if (ok) s.clock.values[std::wstring(rest.substr(0, dot))][std::wstring(rest.substr(dot + 1))] = value;
        } else {
            if (problems) problems->push_back(Format(L"line %zu: unknown key '%.*s'", lineNo, (int)key.size(), key.data()));
            continue;
        }
        if (!ok && problems)
            problems->push_back(Format(L"line %zu: bad value for '%.*s'", lineNo, (int)key.size(), key.data()));
    }
    return s;
}

std::wstring SerializeSettings(const Settings &s) {
    std::wstring out = L"# AnimeLogon settings. Edit through the AnimeLogon settings app.\r\n";
    out += L"monitor_mode = " + std::wstring(ToString(s.monitorMode)) + L"\r\n";
    out += L"scaling = " + std::wstring(ToString(s.scaling)) + L"\r\n";
    out += L"video = " + s.video + L"\r\n";
    for (const auto &[monitor, id] : s.screens) out += kScreenPrefix + monitor + L" = " + id + L"\r\n";
    out += L"audio = " + std::wstring(s.audio.enabled ? L"true" : L"false") + L"\r\n";
    out += L"audio_device = " + (s.audio.device.empty() ? std::wstring(L"auto") : s.audio.device) + L"\r\n";
    out += L"audio_volume = " + std::to_wstring(s.audio.volume) + L"\r\n";
    out += L"video_audio = " + std::wstring(s.audio.videoTrack.enabled ? L"true" : L"false") + L"\r\n";
    out += L"video_audio_volume = " + std::to_wstring(s.audio.videoTrack.volume) + L"\r\n";
    const ClockSettings &c = s.clock;
    out += L"clock = " + std::wstring(c.enabled ? L"true" : L"false") + L"\r\n";
    out += L"clock_displays = " + std::wstring(ToString(c.displays)) + L"\r\n";
    out += L"clock_hours = " + std::wstring(ToString(c.style.hours)) + L"\r\n";
    out += L"clock_ampm = " + std::wstring(c.style.ampm ? L"true" : L"false") + L"\r\n";
    out += L"clock_date = " + std::wstring(ToString(c.style.date)) + L"\r\n";
    out += L"clock_language = " + c.style.locale + L"\r\n";
    out += L"skin = " + c.skin + L"\r\n";
    for (const auto &[id, values] : c.values)
        for (const auto &[setting, value] : values) out += kSkinPrefix + id + L"." + setting + L" = " + value + L"\r\n";
    return out;
}

Settings LoadSettings(bool requireTrusted, std::wstring *why) {
    const std::wstring path = paths::SettingsPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return Settings{};
    std::wstring reason;
    if (requireTrusted && !secure::IsTrusted(path, &reason)) {
        if (why) *why = L"settings.ini " + reason;
        return Settings{};
    }
    // The settings app holds the file exclusively while it rewrites it.
    std::vector<uint8_t> bytes;
    bool read = false;
    for (int attempt = 0; attempt < kReadAttempts && !read; ++attempt) {
        read = secure::ReadFileBytes(path, &bytes, kMaxSettingsBytes);
        if (!read && GetLastError() != ERROR_SHARING_VIOLATION) break;
        if (!read) Sleep(kRetryMs);
    }
    if (!read) {
        if (why) *why = L"settings.ini could not be read";
        return Settings{};
    }
    size_t skip = (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
    std::vector<std::wstring> problems;
    Settings s = ParseSettings(FromUtf8(std::string_view((const char *)bytes.data() + skip, bytes.size() - skip)),
                               &problems);
    for (const std::wstring &p : problems) ALOG(L"settings.ini %s", p.c_str());
    return s;
}

}  // namespace animelogon
