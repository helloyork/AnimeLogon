// When the parked overlay plans the displays again ahead of the next lock. It does so only to get
// scene wallpapers' renderers ready (scenes.h), and planning means reading settings.ini, every
// theme it names and the displays' configuration -- so it is done again only when settings.ini or
// the displays change, not on a timer. While a scene wallpaper is planned it is also done once a
// minute, to retry a renderer that would not start. No threads or windows here, so it is tested
// directly.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace parkedplan {

// What a file looks like from outside: enough to tell that it was rewritten.
struct Stamp {
    bool exists = false;
    uint64_t written = 0;
    uint64_t bytes = 0;
    bool operator==(const Stamp &o) const { return exists == o.exists && written == o.written && bytes == o.bytes; }
    bool operator!=(const Stamp &o) const { return !(*this == o); }
};

inline Stamp StampOf(const std::wstring &path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return s;
    s.exists = true;
    s.written = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    s.bytes = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return s;
}

class Watch {
public:
    static constexpr ULONGLONG kRetryMs = 60000;

    // Whether to plan again now: the first time, when settings.ini (`settings`) or the displays
    // (`layout`) changed since the last plan, or a minute after it while it had a scene.
    bool Due(const Stamp &settings, const std::vector<RECT> &layout, ULONGLONG now) {
        bool same = planned_ && settings == settings_ && layout.size() == layout_.size();
        for (size_t i = 0; same && i < layout.size(); ++i) same = EqualRect(&layout[i], &layout_[i]) != FALSE;
        if (same && !(scene_ && now - at_ >= kRetryMs)) return false;
        planned_ = true;
        settings_ = settings;
        layout_ = layout;
        at_ = now;
        return true;
    }

    // What the plan just made had: a scene wallpaper or not.
    void Planned(bool scene) { scene_ = scene; }
    bool Scene() const { return scene_; }

private:
    bool planned_ = false;
    bool scene_ = false;
    Stamp settings_;
    std::vector<RECT> layout_;
    ULONGLONG at_ = 0;
};

}  // namespace parkedplan
