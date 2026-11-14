// Listens to what an output device plays, for the components' spectra (animelogon/spectrum.h):
// on the logon screen, and in the settings app's previews.
//
// It takes WASAPI's loopback of a device: the one `device` names when given (the logon screen asks
// for the signed-in person's own output, AudioSystem::ListeningDevice), otherwise the default
// output of whoever runs it. A thread keeps the latest second of samples, mixed to one channel, in
// memory only; nothing is recorded or written. While the device plays nothing Windows hands over
// no samples at all, and the thread counts that time as silence.
//
// On the logon screen while nobody is signed in, Windows hands a loopback only silence, however
// loud the device plays; on a signed-in person's locked screen it hands over what is playing.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace animelogon {

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture() { Stop(); }
    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    // Starts listening in the background. `device` gives the id of the device wanted, asked again
    // each time the device is opened; empty, or no function, is the default output. Opening takes
    // a moment, and is tried again every few seconds when it fails or the device goes away.
    void Start(std::function<std::wstring()> device = {});
    // Stops listening. It waits a second at most: a device that hangs does not hold up the caller,
    // and its thread ends on its own when it can.
    void Stop();
    bool Listening() const { return shared_ != nullptr; }

    // The latest samples, oldest first -- as many as spectrum::Analyzer takes at the device's rate
    // -- and that rate. False until the device is open.
    bool Latest(std::vector<float> *samples, float *rate);

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

}  // namespace animelogon
