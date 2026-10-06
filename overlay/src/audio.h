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

    // Loops a 48 kHz, 16-bit stereo WAV file from `position` seconds, fading in.
    bool PlayVideoTrack(const std::wstring &wavPath, double position, float fadeInSeconds);
    void FadeOut(float seconds);
    void Pause();
    void Resume();

    // Seconds of the video track played since it started (its own clock), if playing.
    bool TrackClock(double *seconds) const;

    // Called every frame: finishes opening, feeds buffers, applies fades.
    void Tick();

    // Remembers the signed-in user's default output for the boot screen.
    static void RememberConsoleDefault();

private:
    struct Track;
    void Opened();

    animelogon::AudioSettings settings_;
    struct Pending;
    std::shared_ptr<Pending> pending_;
    HMODULE dll_ = nullptr;
    IXAudio2 *engine_ = nullptr;
    IXAudio2MasteringVoice *master_ = nullptr;
    std::unique_ptr<Track> track_;
    std::wstring wantTrack_;
    double wantPosition_ = 0.0;
    float wantFade_ = 0.0f;
    bool wantPlay_ = false;
    ULONGLONG wantAt_ = 0;
    bool paused_ = false;  // a track that starts now starts paused
};
