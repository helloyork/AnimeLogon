// The data the components show besides the clock, as the logon screen may see it: the
// battery, read here, and what is playing, from live.ini (animelogon/live.h) -- the service's
// copy of what the signed-in person's own session reported.
#pragma once

#include <windows.h>

#include "animelogon/data.h"

class DataFeed {
public:
    // `session` is this overlay's. `media` is the settings' switch: off, nothing playing is shown.
    DataFeed(DWORD session, bool media) : session_(session), media_(media) {}

    // Reads what is due -- the battery each second, live.ini when it changed -- and returns true
    // when the snapshot differs from the last one returned.
    bool Poll(animelogon::data::Snapshot *out);

private:
    animelogon::data::Media ReadMedia();

    DWORD session_;
    bool media_;
    ULONGLONG batteryAt_ = 0, liveAt_ = 0;
    FILETIME liveWritten_{};
    bool liveSeen_ = false;
    bool first_ = true;
    animelogon::data::Snapshot now_;
};
