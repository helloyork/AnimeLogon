#pragma once
#include <map>
#include <memory>
#include <set>
#include "animelogon/scenehost.h"
#include "presenter.h"
// Renderers survive an appearance. A failure is counted once per lock, and stored so a
// graphics reset followed by a new overlay does not erase the consecutive-failure limit.
class Scenes {
public:
    void Prepare(Presenter &presenter, const plan::Plan &plan);
    // Which of the user's frame rate caps applies for this lock, read as it starts: a still picture
    // chosen for it holds for the whole lock. Before anything asks Static().
    void Power(bool onBattery, bool saver);
    void Begin();
    // `fps` is the frame rate cap for the power state now (settings.h FrameRateNow): a scene stops
    // where it is while it is kFpsStill.
    bool Tick(const animelogon::scene::Inputs &inputs, bool draw, float refresh, int fps);
    void Pause();
    void End();
    void Unlocked();
    void Fail(const std::wstring &id, const std::wstring &why);
    void FailAll(const std::wstring &why);
    bool Static(const std::wstring &id) const;
    // Whether a scene playing this lock follows the sound, so the sound is listened to.
    bool Listens() const;
    Presenter::Picture Picture(const plan::Wallpaper &wallpaper) const;
    void Video(const std::wstring &id, Presenter &presenter, const VideoPlayer::Frame &frame, int width, int height);
private:
    struct Entry {
        std::wstring id;
        animelogon::skin::Values values;
        std::unique_ptr<animelogon::scene::Host> host;
        int width = 0, height = 0, failures = 0;
        ULONGLONG last = 0, fade = 0;
        bool failed = false, active = false, everReady = false;
        bool blocked = false;
        bool staticOnly = false;
        bool hears = false;  // the scene reads audio.* (scenegraph::ReadsSound)
        unsigned frames = 0; // new frames this lock, for the log
        // The quality, video memory and particle settings the renderer was started with.
        int quality = -1, videoMemory = -1, particles = -1;
        // The video goes over whole: the renderer fits it into its node.
        animelogon::Scaling fit = animelogon::Scaling::Stretch;
    };
    std::map<std::wstring, Entry> entries_;
    animelogon::RenderSettings render_;
    // The power state at the start of this lock asked for the still picture.
    bool stillThisLock_ = false;
    bool live_ = false, healthRead_ = false;
    std::map<std::wstring, int> saved_;
    std::set<std::wstring> savedBlocked_;
    std::string savedText_;
    void Save();
};
