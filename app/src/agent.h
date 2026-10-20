// animelogon.exe --agent: runs in the signed-in person's own session, started and ended by the
// service. While the theme in use shows what is playing and settings.ini's `media` is on, it
// reads Windows' media controls (the same ones its volume flyout shows) and sends what it finds
// to the service, which keeps it for the lock screen (animelogon/live.h). No window, no network.
#pragma once
#include <memory>
#include "animelogon/data.h"

namespace app {

int RunAgent();
class PreviewMedia {
public:
    PreviewMedia();
    ~PreviewMedia();
    animelogon::data::Media Get(bool enabled, bool *ready = nullptr);
private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace app
