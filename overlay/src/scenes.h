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
    void Begin();
    bool Tick(const animelogon::scene::Inputs &inputs, bool draw, float refresh);
    void Pause();
    void End();
    void Unlocked();
    void Fail(const std::wstring &id, const std::wstring &why);
    void FailAll(const std::wstring &why);
    Presenter::Picture Picture(const plan::Wallpaper &wallpaper) const;
    void Video(const std::wstring &id, Presenter &presenter, const VideoPlayer::Frame &frame, int width, int height);
private:
    struct Entry {
        std::unique_ptr<animelogon::scene::Host> host;
        int width = 0, height = 0, failures = 0;
        ULONGLONG last = 0, fade = 0;
        bool failed = false, active = false, everReady = false;
        bool blocked = false;
        animelogon::Scaling fit = animelogon::Scaling::Fill;
    };
    std::map<std::wstring, Entry> entries_;
    bool live_ = false, healthRead_ = false;
    std::map<std::wstring, int> saved_;
    std::set<std::wstring> savedBlocked_;
    std::string savedText_;
    void Save();
};
