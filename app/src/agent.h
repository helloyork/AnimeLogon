// animelogon.exe --agent: runs in the signed-in person's own session, started and ended by the
// service. While the theme in use shows what is playing and settings.ini's `media` is on, it
// reads Windows' media controls (the same ones its volume flyout shows) and sends what it finds
// to the service, which keeps it for the lock screen (animelogon/live.h). No window, no network.
#pragma once

namespace app {

int RunAgent();

}  // namespace app
