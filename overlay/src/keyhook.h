// The low-level keyboard hook that withholds the wake key (see wake.h). It runs on its own
// thread, which does nothing but pump messages, so it answers within the system's hook
// timeout even while the render thread is busy. It is installed only while the video is up
// or about to come back, and removed otherwise.
//
// Privacy: it withholds at most one key per screen -- the one that wakes it -- and passes
// every other key straight to Windows untouched. Withheld keys are counted, never recorded
// by identity.
#pragma once

#include <windows.h>

#include <atomic>

#include "wake.h"

namespace keyhook {

// Posted to the notify window when a withheld key wakes the screen.
constexpr UINT kWakeMessage = WM_APP + 1;

struct Shared {
    std::atomic<bool> armed{false};      // the video is up and nobody has woken it yet
    std::atomic<bool> displayOff{false};
    std::atomic<HWND> notify{nullptr};
    std::atomic<long> withheld{0};       // counted, never named
};

Shared &State();

bool Install();
void Uninstall();
bool Installed();

// A new screen: the one key is not spent yet.
void ResetLatch();
// The panel went dark: the next key lights it rather than waking.
void DarkenLatch();

}  // namespace keyhook
