#include "scenes.h"
#include <algorithm>
#include <set>
#include "animelogon/data.h"
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"
using namespace animelogon;
namespace {
std::wstring Key(const plan::Wallpaper &w) {
    std::wstring key = w.id;
    for (const auto &[name,value] : w.values) key += L"\n"+name+L"="+value;
    return key;
}
std::wstring Health() { return paths::DataDir() + L"\\scene-health.ini"; }
}
void Scenes::Save() {
    std::string text;
    std::wstring why;
    for (const auto &[id,n] : saved_) text += ToUtf8(id)+"="+std::to_string(n)+(savedBlocked_.count(id) ? ",1\n" : ",0\n");
    // A development overlay is not elevated and cannot change the real store.
    if (text != savedText_ && secure::IsTrustedDirectory(paths::DataDir(), &why) &&
        secure::WriteBytes(Health(), text.data(), text.size()) == ERROR_SUCCESS) savedText_ = std::move(text);
}
void Scenes::Prepare(Presenter &presenter, const plan::Plan &plan) {
    if (!healthRead_) {
        healthRead_ = true; std::vector<uint8_t> bytes; std::wstring why;
        if (secure::IsTrusted(Health(), &why) && secure::ReadFileBytes(Health(), &bytes, 4096)) {
            savedText_.assign((const char *)bytes.data(),bytes.size());
            const std::wstring text = FromUtf8(std::string_view((const char *)bytes.data(),bytes.size()));
            size_t start = 0;
            while (start < text.size()) {
                const size_t end = text.find(L'\n',start), eq = text.find(L'=',start);
                if (end == std::wstring::npos) break;
                long long n = 0;
                const size_t comma = text.find(L',',eq < end ? eq : end);
                const size_t valueEnd = std::min(end,comma);
                if (eq < end && IsWallpaperId(text.substr(start,eq-start)) && ParseInt(std::wstring_view(text).substr(eq+1,valueEnd-eq-1),&n) && n >= 0 && n <= 3) {
                    const auto id = text.substr(start,eq-start); saved_[id] = (int)n;
                    if (comma < end && text.substr(comma+1,end-comma-1) == L"1") savedBlocked_.insert(id);
                }
                start = end+1;
            }
        }
    }
    render_ = plan.render;
    std::map<std::wstring, std::pair<int,int>> wanted;
    std::map<std::wstring, const plan::Wallpaper *> wallpapers;
    for (const auto &d : plan.displays) if (d.wallpaper.source == plan::Source::Scene) {
        auto &size = wanted[Key(d.wallpaper)];
        const int w = d.canvas.right-d.canvas.left, h = d.canvas.bottom-d.canvas.top;
        if ((int64_t)w*h > (int64_t)size.first*size.second) size = {w,h};
        wallpapers[Key(d.wallpaper)] = &d.wallpaper;
        presenter.RequestImage(d.wallpaper.path,d.wallpaper.path);
    }
    for (auto it = saved_.begin(); it != saved_.end();) {
        if (std::none_of(wallpapers.begin(),wallpapers.end(),[&](const auto &w) { return w.second->id == it->first; })) { savedBlocked_.erase(it->first); it = saved_.erase(it); } else ++it;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!wanted.count(it->first)) { it = entries_.erase(it); } else ++it;
    }
    for (const auto &[id, size] : wanted) {
        auto [it, added] = entries_.try_emplace(id); auto &e = it->second;
        if (added) { e.id = wallpapers[id]->id; e.values = wallpapers[id]->values; e.failures = saved_[e.id]; e.blocked = savedBlocked_.count(e.id) != 0; }
        const bool sameSettings = e.quality == (int)render_.quality && e.videoMemory == render_.videoMemory && e.particles == render_.particles;
        if (e.host && e.width == size.first && e.height == size.second && sameSettings && !e.blocked) continue;
        e.host.reset(); e.width = size.first; e.height = size.second;
        e.quality = (int)render_.quality; e.videoMemory = render_.videoMemory; e.particles = render_.particles;
        // The import trial's verdict that the PC is too slow belongs to the automatic quality; a
        // quality the user fixed plays the scene and drops frames instead.
        e.staticOnly = wallpapers[id]->sceneFps == 0 && render_.quality == RenderQuality::Auto;
        if (e.staticOnly) {
            if (added) ALOG(L"scene: %s uses its static image because the import trial exceeded the frame budget at half resolution",e.id.c_str());
            continue;
        }
        if (e.failures >= 3) {
            if (added) ALOG(L"scene: %s disabled after three consecutive failed locks; waiting for a wallpaper change",e.id.c_str());
            continue;
        }
        if (e.blocked) continue;
        scene::Files files; scenegraph::Document document; std::wstring why;
        auto host = std::make_unique<scene::Host>();
        const auto &w = *wallpapers[id];
        scene::Options options;
        options.tier = {w.sceneTrialWidth > 0,w.sceneFps,w.sceneScale/100.f};
        if (options.tier.calibrated) {
            const double ratio = std::sqrt((double)w.sceneTrialWidth*w.sceneTrialHeight/((double)e.width*e.height));
            options.tier.scale = std::clamp((float)(options.tier.scale*ratio),.5f,1.f);
        }
        options.quality = render_.quality; options.videoMemory = render_.videoMemory; options.particles = render_.particles;
        // The user's values become the scene's defaults, so the renderer needs nothing else.
        if (scene::LoadWithValues(wallpapers[id]->sceneDir,e.values,&files,&document,&why) &&
            host->Start(presenter.device(),files,e.width,e.height,options,&why)) {
            e.host = std::move(host); e.last = GetTickCount64(); e.everReady = false;
            e.hears = scenegraph::ReadsSound(document);
            e.failed = false;
        } else {
            ALOG(L"scene: %s could not be prepared (%s); its static image remains available",e.id.c_str(),why.c_str());
            e.failed = true;
        }
    }
    Save();
}
void Scenes::Power(bool onBattery, bool saver) {
    stillThisLock_ = FrameRateNow(render_, onBattery, saver) == kFpsStill;
    if (stillThisLock_ && !entries_.empty()) ALOG(L"scene: the still picture for this lock, as the frame rate setting for this power state asks");
}
void Scenes::Begin() {
    live_ = true;
    for (auto &[id,e] : entries_) {
        e.active = e.failures < 3 && !e.blocked && !e.staticOnly && !stillThisLock_; e.fade = 0; e.last = GetTickCount64(); e.frames = 0;
        if (e.active && e.failed) { e.failed = false; Fail(e.id,L"renderer could not be prepared"); }
    }
}
void Scenes::Fail(const std::wstring &id, const std::wstring &why) {
    if (savedBlocked_.count(id)) return;
    bool active = false;
    for (const auto &[key,e] : entries_) if (e.id == id && e.active && !e.failed) active = true;
    if (!active) return;
    const int failures = saved_[id] = std::min(3,saved_[id]+1); savedBlocked_.insert(id);
    for (auto &[key,e] : entries_) if (e.id == id) {
        e.failed = true; e.active = false; e.blocked = true; e.host.reset(); e.failures = failures;
    }
    Save();
    ALOG(L"scene: %s stopped (%s); static image for this lock, consecutive failures %d",id.c_str(),why.c_str(),failures);
    if (failures == 3) ALOG(L"scene: %s disabled after three consecutive failed locks; waiting for a wallpaper change",id.c_str());
}
void Scenes::FailAll(const std::wstring &why) { for (const auto &[id,e] : entries_) Fail(e.id,why); }
bool Scenes::Listens() const {
    for (const auto &[id,e] : entries_) if (e.active && e.hears && e.host) return true;
    return false;
}
bool Scenes::Static(const std::wstring &id) const {
    for (const auto &[key,e] : entries_) if (e.id == id && (e.staticOnly || e.failed || e.blocked || e.failures >= 3 || stillThisLock_)) return true;
    return false;
}
bool Scenes::Tick(const scene::Inputs &inputs, bool draw, float refresh, int fps) {
    bool changed = false; const ULONGLONG now = GetTickCount64();
    // Drawing faster than the display shows frames gains nothing.
    const float rate = fps == kFpsStill ? 0.f : fps == kFpsDisplay ? refresh : std::min((float)fps, refresh);
    const bool moving = draw && rate > 0;
    for (auto &[id,e] : entries_) {
        if (!e.host || e.failed) continue;
        // The values are already the scene's defaults (Bake), so none go with the inputs.
        e.host->SetRunning(live_ && e.active && moving,inputs,rate > 0 ? rate : 1.f);
        if (e.host->Poll()) { e.last = now; changed = live_ || changed; if (live_) ++e.frames; }
        if (e.host->Ready()) {
            if (!e.everReady) ALOG(L"scene: %s ready, trial p95 %.2f ms",e.id.c_str(),e.host->Milliseconds());
            e.everReady = true;
            if (live_ && !e.fade) { e.fade = now; changed = true; }
        }
        std::wstring why;
        if (live_ && e.active && (e.host->Failed(&why) || (moving && now-e.last > 2000))) {
            if (why.empty()) why = L"no new frame for two seconds";
            Fail(e.id,why); changed = true;
        }
        if (!moving) e.last = now;
        if (e.fade && now-e.fade < 400) changed = true;
    }
    return changed;
}
void Scenes::Pause() {
    live_ = false;
    for (auto &[id,e] : entries_) if (e.host) e.host->SetRunning(false,{});
}
void Scenes::End() {
    for (const auto &[key,e] : entries_) if (e.active && e.everReady) ALOG(L"scene: %s drew %u frames this lock",e.id.c_str(),e.frames);
    Pause();
    for (auto &[key,e] : entries_) if (e.active && e.everReady && !e.failed && !savedBlocked_.count(e.id)) {
        saved_[e.id] = 0; e.failures = 0;
    }
    Save();
}
void Scenes::Unlocked() {
    bool changed = !savedBlocked_.empty(); savedBlocked_.clear();
    for (auto &[id,e] : entries_) if (e.blocked) { e.blocked = false; e.failed = false; changed = true; }
    if (changed) Save();
}
Presenter::Picture Scenes::Picture(const plan::Wallpaper &w) const {
    Presenter::Picture p; p.kind = Presenter::Picture::Kind::Scene;
    const auto it = entries_.find(Key(w));
    if (it == entries_.end()) return p;
    const auto &e = it->second;
    if (live_ && e.host && e.active && !e.failed && e.host->Ready() && e.fade) {
        p.scene = e.host->View(); p.sceneW = e.width; p.sceneH = e.height;
        p.sceneMix = std::min(1.f,(GetTickCount64()-e.fade)/400.f);
    }
    return p;
}
void Scenes::Video(const std::wstring &id, Presenter &presenter, const VideoPlayer::Frame &frame, int width, int height) {
    for (auto &[key,e] : entries_) if (e.id == id && e.host && e.active) {
        e.host->UpdateVideo([&](ID3D11Texture2D *texture) { presenter.DrawVideo(frame,width,height,e.fit,texture); });
    }
}
