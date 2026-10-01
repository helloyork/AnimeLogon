// settings.ini: the machine-wide configuration. Written by the settings app's elevated
// helper, read by the overlay. One `key = value` per line; unknown keys are ignored.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "animelogon/clock.h"

namespace animelogon {

enum class MonitorMode {
    Duplicate,  // the same video on every display
    Span,       // one video across all displays
    PerMonitor, // each display plays its own video
};

enum class Scaling {
    Fill,     // cover the display, cropping the edges
    Fit,      // show the whole video, with bars
    Stretch,  // fill the display, ignoring the aspect ratio
};

struct AudioSource {
    bool enabled = true;
    int volume = 100;  // 0..100
};

struct AudioSettings {
    bool enabled = false;
    std::wstring device;  // empty: follow the signed-in user's default device
    int volume = 80;      // 0..100
    AudioSource videoTrack;
};

struct Settings {
    MonitorMode monitorMode = MonitorMode::Duplicate;
    Scaling scaling = Scaling::Fill;
    std::wstring video;                          // library id; used unless a display has its own
    std::map<std::wstring, std::wstring> screens; // monitor key -> library id (PerMonitor)
    AudioSettings audio;
    ClockSettings clock;

    // The video a display should play, or empty for none.
    std::wstring VideoFor(const std::wstring &monitorKey) const;
};

// Parses the file's text. Malformed lines are reported in `problems` and ignored.
Settings ParseSettings(const std::wstring &text, std::vector<std::wstring> *problems = nullptr);
std::wstring SerializeSettings(const Settings &settings);

// Reads settings.ini. Returns defaults if it is missing, and refuses a file that is not
// administrators-only when `requireTrusted` is set.
Settings LoadSettings(bool requireTrusted, std::wstring *why = nullptr);

const wchar_t *ToString(MonitorMode mode);
const wchar_t *ToString(Scaling scaling);

}  // namespace animelogon
