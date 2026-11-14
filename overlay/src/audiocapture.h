// Listens to what the output device plays, for the components' spectra (animelogon/spectrum.h).
//
// It takes WASAPI's loopback of the device the signed-in person's sound goes to -- their default
// output, where their music plays while the screen is locked -- or, with nobody signed in, the one
// remembered from the last sign-in, or the system's default (AudioSystem::ListeningDevice). The
// wallpaper's own sound track, when it plays, is heard the same way. A thread keeps the latest
// second of samples, mixed to one channel, in memory only; nothing is recorded or written. While
// the device plays nothing Windows hands over no samples at all, and the thread counts that time
// as silence.
#pragma once

#include <windows.h>

#include <memory>
#include <vector>

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture() { Stop(); }
    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    // Starts listening in the background. Opening the device takes a moment, and is tried again
    // every few seconds when it fails or the device goes away.
    void Start();
    // Stops listening. It waits a second at most: a device that hangs does not hold up the logon
    // screen's way out, and its thread ends on its own when it can.
    void Stop();

    // The latest samples, oldest first -- as many as spectrum::Analyzer takes at the device's rate
    // -- and that rate. False until the device is open.
    bool Latest(std::vector<float> *samples, float *rate);

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};
