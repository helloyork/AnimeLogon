#include "plan.h"

namespace plan {

std::vector<Presenter::Target> Build(const animelogon::Settings &settings,
                                     const std::vector<animelogon::MonitorInfo> &monitors) {
    std::vector<Presenter::Target> targets;
    if (monitors.empty()) return targets;

    RECT bounds = animelogon::VirtualBounds(monitors);
    const bool span = settings.monitorMode == animelogon::MonitorMode::Span;
    // Spanning needs a video: use the primary display's (or the default) for the whole wall.
    const std::wstring spanVideo = settings.VideoFor(monitors.front().key);

    for (const animelogon::MonitorInfo &m : monitors) {
        Presenter::Target t;
        t.rect = m.rect;
        if (span) {
            t.canvas = bounds;
            t.videoId = spanVideo;
        } else {
            t.canvas = m.rect;
            t.videoId = settings.VideoFor(m.key);
        }
        targets.push_back(std::move(t));
    }
    return targets;
}

}  // namespace plan
