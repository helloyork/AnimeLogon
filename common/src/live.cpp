#include "animelogon/live.h"

#include <cmath>
#include <set>

#include "animelogon/paths.h"
#include "animelogon/text.h"

namespace animelogon::live {
namespace {

constexpr size_t kMaxLine = 1024;
// Ten days, in milliseconds: longer than any track or stream position worth showing.
constexpr unsigned long long kMaxMs = 10ull * 24 * 3600 * 1000;

bool Digits(std::string_view s, unsigned long long max, unsigned long long *out) {
    if (s.empty() || s.size() > 20) return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const unsigned long long d = (unsigned long long)(c - '0');
        if (v > (max - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

std::string Ms(double seconds) {
    const double ms = std::round(seconds * 1000.0);
    return std::to_string(ms <= 0.0 ? 0ull : ms >= (double)kMaxMs ? kMaxMs : (unsigned long long)ms);
}

}  // namespace

std::wstring Path() { return paths::DataDir() + L"\\live.ini"; }

std::string Serialize(const Live &live, bool withSession) {
    const data::Media &m = live.media;
    std::string out = "format=1\n";
    if (withSession) out += "session=" + std::to_string(live.session) + "\n";
    const char *state = m.state == data::MediaState::Playing ? "playing" : m.state == data::MediaState::Paused ? "paused" : "none";
    out += std::string("media.state=") + state + "\n";
    if (m.state == data::MediaState::None) return out;
    const struct {
        const char *key;
        const std::wstring &value;
    } texts[] = {{"media.title", m.title}, {"media.artist", m.artist}, {"media.album", m.album}, {"media.app", m.app}};
    for (const auto &t : texts) {
        const std::wstring clean = data::CleanText(t.value);
        if (!clean.empty()) out += std::string(t.key) + "=" + ToUtf8(clean) + "\n";
    }
    out += "media.position-ms=" + Ms(m.position) + "\n";
    if (m.duration > 0.0) out += "media.duration-ms=" + Ms(m.duration) + "\n";
    const double rate = std::round(m.rate * 1000.0);
    out += "media.rate-permille=" + std::to_string(rate <= 0.0 ? 0 : rate >= 100000.0 ? 100000 : (int)rate) + "\n";
    if (m.at) out += "media.at=" + std::to_string(m.at) + "\n";
    return out;
}

bool Parse(std::string_view text, Live *out, std::wstring *why) {
    auto fail = [&](const wchar_t *what) {
        if (why) *why = what;
        return false;
    };
    if (text.size() > kMaxBytes) return fail(L"too long");
    Live live;
    std::set<std::string> seen;
    bool format = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.size() > kMaxLine) return fail(L"a line is too long");
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return fail(L"a line is not key=value");
        const std::string key(line.substr(0, eq));
        const std::string_view value = line.substr(eq + 1);
        if (!seen.insert(key).second) return fail(L"a key is repeated");
        unsigned long long n = 0;
        data::Media &m = live.media;
        if (key == "format") {
            if (value != "1") return fail(L"unsupported format");
            format = true;
        } else if (key == "session") {
            if (!Digits(value, 0xFFFFFFFEull, &n)) return fail(L"a bad session");
            live.session = (DWORD)n;
        } else if (key == "media.state") {
            if (value == "playing") m.state = data::MediaState::Playing;
            else if (value == "paused") m.state = data::MediaState::Paused;
            else if (value == "none") m.state = data::MediaState::None;
            else return fail(L"a bad media.state");
        } else if (key == "media.title" || key == "media.artist" || key == "media.album" || key == "media.app") {
            const std::wstring clean = data::CleanText(FromUtf8(value));
            (key == "media.title" ? m.title : key == "media.artist" ? m.artist : key == "media.album" ? m.album : m.app) = clean;
        } else if (key == "media.position-ms" || key == "media.duration-ms") {
            if (!Digits(value, kMaxMs, &n)) return fail(L"a bad time");
            (key == "media.position-ms" ? m.position : m.duration) = (double)n / 1000.0;
        } else if (key == "media.rate-permille") {
            if (!Digits(value, 100000, &n)) return fail(L"a bad rate");
            m.rate = (double)n / 1000.0;
        } else if (key == "media.at") {
            if (!Digits(value, ~0ull, &n)) return fail(L"a bad time stamp");
            m.at = n;
        } else {
            return fail(L"an unknown key");
        }
    }
    if (!format) return fail(L"no format");
    if (live.media.state == data::MediaState::None) live.media = data::Media{};
    *out = std::move(live);
    return true;
}

}  // namespace animelogon::live
