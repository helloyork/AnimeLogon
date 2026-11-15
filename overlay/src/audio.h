// The logon screen's audio system. Sources are mixed into one output device; the video's
// sound track is one source. Everything plays through XAudio2 in the SoundEffects category,
// the only kind Windows lets through before anybody signs in.
#pragma once

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "animelogon/settings.h"
#include "soundyield.h"

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem &) = delete;
    AudioSystem &operator=(const AudioSystem &) = delete;

    // Starts opening the output device in the background; it may wait a few seconds for a
    // USB device at boot.
    void Open(const animelogon::AudioSettings &settings);
    void Close();
    bool ready() const { return engine_ != nullptr; }

    // Loops a 48 kHz, 16-bit stereo WAV file from `position` seconds, fading in, every
    // `loopSeconds` -- the video's own length, so the two stay together (soundloop.h); 0 for the
    // file's own length. Before the
    // output is open it is only remembered, and starts once it is from where the picture is then
    // (Tick). While other programs make sound and the settings say video_audio_yield, it plays
    // muted, and comes back once they have been quiet for a few seconds; a program that only
    // repeats it, listening to the output, does not count (soundyield.h).
    bool PlayVideoTrack(const std::wstring &wavPath, double position, float fadeInSeconds, double loopSeconds);
    void FadeOut(float seconds);
    void Pause();
    void Resume();

    // Seconds of the video track played since it started (its own clock), if playing.
    bool TrackClock(double *seconds) const;
    // What the video track is playing now: its last count samples, mixed to one channel and as
    // loud as they reach the device, and their rate. False when no track is playing. Before anybody
    // has signed in, Windows lets this sound through to the speakers but hands a loopback only
    // silence, so the spectrum reads it from here (overlay.cpp).
    bool Recent(std::vector<float> *mono, size_t count, float *rate);

    // Called every frame: finishes opening, feeds buffers, applies fades. `pictureClock` is the
    // video's own clock, in seconds: where a remembered track starts once the output is open.
    void Tick(double pictureClock);

    // Remembers the signed-in user's default output for the boot screen.
    static void RememberConsoleDefault();
    // Where the signed-in user's sound goes: their default output, else the one remembered from the
    // last sign-in; empty for the system's default. What a spectrum listens to (animelogon/audiocapture.h).
    // Needs COM.
    static std::wstring ListeningDevice();

private:
    struct Track;
    struct Watch;
    void Opened(double pictureClock);

    animelogon::AudioSettings settings_;
    struct Pending;
    std::shared_ptr<Pending> pending_;
    HMODULE dll_ = nullptr;
    IXAudio2 *engine_ = nullptr;
    IXAudio2MasteringVoice *master_ = nullptr;
    std::unique_ptr<Track> track_;
    std::wstring wantTrack_;
    double wantLoop_ = 0.0;
    float wantFade_ = 0.0f;
    bool wantPlay_ = false;
    bool paused_ = false;  // a track that starts now starts paused
    // Other programs' sound on the device (video_audio_yield), watched by the opening thread, and
    // when the track gives way to it (soundyield.h).
    std::shared_ptr<Watch> watch_;
    soundyield::Rule yield_;
    uint64_t looksTaken_ = 0;  // the watch's looks seen so far
};
