#include "scenes.h"
#include <algorithm>
#include <set>
#include "animelogon/log.h"
#include "animelogon/paths.h"
#include "animelogon/secure.h"
#include "animelogon/text.h"
#include "animelogon/wallpaper.h"
using namespace animelogon;
namespace {
std::wstring Health() { return paths::DataDir() + L"\\scene-health.ini"; }
}
void Scenes::Save() {
    std::string text;
    std::wstring why;
    for (const auto &[id, e] : entries_) text += ToUtf8(id) + "=" + std::to_string(e.failures) + (e.blocked ? ",1\n" : ",0\n");
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
    std::map<std::wstring, std::pair<int,int>> wanted;
    std::map<std::wstring, const plan::Wallpaper *> wallpapers;
    for (const auto &d : plan.displays) if (d.wallpaper.source == plan::Source::Scene) {
        auto &size = wanted[d.wallpaper.id];
        const int w = d.canvas.right-d.canvas.left, h = d.canvas.bottom-d.canvas.top;
        if ((int64_t)w*h > (int64_t)size.first*size.second) size = {w,h};
        wallpapers[d.wallpaper.id] = &d.wallpaper;
        presenter.RequestImage(d.wallpaper.id,d.wallpaper.path);
    }
    for (auto it = saved_.begin(); it != saved_.end();) {
        if (!wanted.count(it->first)) { savedBlocked_.erase(it->first); it = saved_.erase(it); } else ++it;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!wanted.count(it->first)) { saved_.erase(it->first); it = entries_.erase(it); } else ++it;
    }
    for (const auto &[id, size] : wanted) {
        auto [it, added] = entries_.try_emplace(id); auto &e = it->second;
        if (added) { e.failures = saved_[id]; e.blocked = savedBlocked_.count(id) != 0; }
        if (e.host && e.width == size.first && e.height == size.second) continue;
        e.host.reset(); e.width = size.first; e.height = size.second; e.fit = wallpapers[id]->fit;
        e.staticOnly = wallpapers[id]->sceneFps == 0;
        if (e.staticOnly) {
            if (added) ALOG(L"scene: %s uses its static image because the import trial exceeded the frame budget at half resolution",id.c_str());
            continue;
        }
        if (e.failures >= 3) {
            if (added) ALOG(L"scene: %s disabled after three consecutive failed locks; waiting for a wallpaper change",id.c_str());
            continue;
        }
        if (e.blocked) continue;
        scene::Files files; scene::Document document; std::wstring why;
        auto host = std::make_unique<scene::Host>();
        const auto &w = *wallpapers[id];
        scene::Tier tier{w.sceneTrialWidth > 0,w.sceneFps,w.sceneScale/100.f};
        if (tier.calibrated) {
            const double ratio = std::sqrt((double)w.sceneTrialWidth*w.sceneTrialHeight/((double)e.width*e.height));
            tier.scale = std::clamp((float)(tier.scale*ratio),.5f,1.f);
        }
        if (scene::LoadFiles(wallpapers[id]->sceneDir,true,&files,&document,&why) &&
            host->Start(presenter.device(),files,e.width,e.height,false,&why,tier)) {
            e.fit = document.base.fit;
            e.host = std::move(host); e.last = GetTickCount64(); e.everReady = false;
            e.failed = false;
        } else {
            ALOG(L"scene: %s could not be prepared (%s); its static image remains available",id.c_str(),why.c_str());
            e.failed = true;
        }
    }
    Save();
}
void Scenes::Begin() {
    live_ = true;
    for (auto &[id,e] : entries_) {
        e.active = e.failures < 3 && !e.blocked && !e.staticOnly; e.fade = 0; e.last = GetTickCount64();
        if (e.active && e.failed) { e.failed = false; Fail(id,L"renderer could not be prepared"); }
    }
}
void Scenes::Fail(const std::wstring &id, const std::wstring &why) {
    const auto it = entries_.find(id); if (it == entries_.end()) return;
    auto &e = it->second;
    if (e.failed || !e.active) return;
    e.failed = true; e.active = false; e.blocked = true; e.host.reset(); e.failures = std::min(3,e.failures+1); Save();
    ALOG(L"scene: %s stopped (%s); static image for this lock, consecutive failures %d",id.c_str(),why.c_str(),e.failures);
    if (e.failures == 3) ALOG(L"scene: %s disabled after three consecutive failed locks; waiting for a wallpaper change",id.c_str());
}
void Scenes::FailAll(const std::wstring &why) { for (const auto &[id,e] : entries_) Fail(id,why); }
bool Scenes::Static(const std::wstring &id) const {
    const auto it = entries_.find(id);
    return it != entries_.end() && (it->second.staticOnly || it->second.failed || it->second.blocked || it->second.failures >= 3);
}
bool Scenes::Tick(const scene::Inputs &inputs, bool draw, float refresh) {
    bool changed = false; const ULONGLONG now = GetTickCount64();
    for (auto &[id,e] : entries_) {
        if (!e.host || e.failed) continue;
        e.host->SetRunning(live_ && e.active && draw,inputs,refresh);
        if (e.host->Poll()) { e.last = now; changed = live_ || changed; }
        if (e.host->Ready()) {
            if (!e.everReady) ALOG(L"scene: %s ready, trial p95 %.2f ms",id.c_str(),e.host->Milliseconds());
            e.everReady = true;
            if (live_ && !e.fade) { e.fade = now; changed = true; }
        }
        std::wstring why;
        if (live_ && e.active && (e.host->Failed(&why) || (draw && now-e.last > 2000))) {
            if (why.empty()) why = L"no new frame for two seconds";
            Fail(id,why); changed = true;
        }
        if (!draw) e.last = now;
        if (e.fade && now-e.fade < 400) changed = true;
    }
    return changed;
}
void Scenes::Pause() {
    live_ = false;
    for (auto &[id,e] : entries_) if (e.host) e.host->SetRunning(false,{});
}
void Scenes::End() {
    Pause();
    for (auto &[id,e] : entries_) if (e.active && e.everReady && !e.failed) e.failures = 0;
    Save();
}
void Scenes::Unlocked() {
    bool changed = !savedBlocked_.empty(); savedBlocked_.clear();
    for (auto &[id,e] : entries_) if (e.blocked) { e.blocked = false; e.failed = false; changed = true; }
    if (changed) Save();
}
Presenter::Picture Scenes::Picture(const plan::Wallpaper &w) const {
    Presenter::Picture p; p.kind = Presenter::Picture::Kind::Scene;
    const auto it = entries_.find(w.id);
    if (it == entries_.end()) return p;
    const auto &e = it->second;
    if (live_ && e.host && e.active && !e.failed && e.host->Ready() && e.fade) {
        p.scene = e.host->View(); p.sceneW = e.width; p.sceneH = e.height;
        p.sceneMix = std::min(1.f,(GetTickCount64()-e.fade)/400.f);
    }
    return p;
}
void Scenes::Video(const std::wstring &id, Presenter &presenter, const VideoPlayer::Frame &frame, int width, int height) {
    auto it = entries_.find(id);
    if (it != entries_.end() && it->second.host && it->second.active) {
        auto &e = it->second;
        e.host->UpdateVideo([&](ID3D11Texture2D *texture) { presenter.DrawVideo(frame,width,height,e.fit,texture); });
    }
}
