#include "datafeed.h"

#include <string>
#include <vector>

#include "animelogon/live.h"
#include "animelogon/log.h"
#include "animelogon/secure.h"
#include "screen.h"

namespace {

using namespace animelogon;

constexpr ULONGLONG kBatteryMs = 1000, kLiveMs = 500;

bool Same(const data::Snapshot &a, const data::Snapshot &b) {
    const data::Battery &x = a.battery, &y = b.battery;
    const data::Media &m = a.media, &n = b.media;
    return x.state == y.state && x.percent == y.percent && x.secondsLeft == y.secondsLeft && m.state == n.state &&
           m.title == n.title && m.artist == n.artist && m.album == n.album && m.app == n.app &&
           m.position == n.position && m.duration == n.duration && m.rate == n.rate && m.at == n.at;
}

}  // namespace

data::Media DataFeed::ReadMedia() {
    static std::wstring said;  // the last reason logged, so a lasting one is logged once
    auto refuse = [&](const std::wstring &why) {
        if (why != said) ALOG(L"data: nothing playing is shown -- %s", why.c_str());
        said = why;
        return data::Media{};
    };
    const std::wstring path = live::Path();
    std::wstring why;
    std::vector<uint8_t> bytes;
    if (!secure::IsTrusted(path, &why)) return refuse(L"live.ini " + why);
    if (!secure::ReadFileBytes(path, &bytes, live::kMaxBytes)) return refuse(L"live.ini could not be read");
    live::Live l;
    if (!live::Parse(std::string_view((const char *)bytes.data(), bytes.size()), &l, &why))
        return refuse(L"live.ini " + why);
    // Only on the lock screen of the session that reported it, with somebody signed in to it.
    if (l.session != session_) return refuse(L"live.ini is another session's");
    if (!screen::ReadSession(session_).signedIn) return refuse(L"nobody is signed in to this session");
    if (said != L"shown") ALOG(L"data: what is playing comes from live.ini");
    said = L"shown";
    return l.media;
}

bool DataFeed::Poll(data::Snapshot *out) {
    const ULONGLONG now = GetTickCount64();
    data::Snapshot next = now_;
    if (first_ || now - batteryAt_ >= kBatteryMs) {
        batteryAt_ = now;
        next.battery = data::ReadBattery();
    }
    if (media_ && (first_ || now - liveAt_ >= kLiveMs)) {
        liveAt_ = now;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const bool exists = GetFileAttributesExW(live::Path().c_str(), GetFileExInfoStandard, &a) != FALSE;
        const bool changed = exists != liveSeen_ || (exists && CompareFileTime(&a.ftLastWriteTime, &liveWritten_) != 0);
        if (first_ || changed) {
            liveSeen_ = exists;
            liveWritten_ = exists ? a.ftLastWriteTime : FILETIME{};
            next.media = exists ? ReadMedia() : data::Media{};
        }
    }
    const bool differs = first_ || !Same(next, now_);
    first_ = false;
    now_ = next;
    if (differs) *out = now_;
    return differs;
}
