// Turning the settings and the attached displays into a list of windows to draw.
#pragma once

#include <vector>

#include "animelogon/monitors.h"
#include "animelogon/settings.h"
#include "presenter.h"

namespace plan {

// One Presenter::Target per display, each with the video it should show and the canvas the
// video is fitted to (the display itself, or the bounds of all of them when spanning).
std::vector<Presenter::Target> Build(const animelogon::Settings &settings,
                                     const std::vector<animelogon::MonitorInfo> &monitors);

}  // namespace plan
