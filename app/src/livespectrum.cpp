#include "livespectrum.h"

#include "animelogon/components.h"
#include "animelogon/log.h"

#include "library.h"
#include "pictures.h"

namespace app {

using namespace animelogon;

namespace {

// How long the device stays open after the page last asked for a frame, and how long a panel's
// view is kept after it was last drawn.
constexpr ULONGLONG kListenMs = 3000, kKeepMs = 5000;
// Frames asked for closer together than this share one look at the sound.
constexpr ULONGLONG kAnalyseMs = 8;

// An instance's component with the values it takes, as the logon screen resolves it.
bool ResolveInstance(const Library &library, const theme::Instance &i, skin::Resolved *out) {
    skin::Skin skin;
    if (!library.LoadSkin(i.ref, &skin)) return false;
    skin::Values values;
    for (const auto &[key, value] : i.sets)
        if (skin::Accepts(skin, key, value)) values[key] = value;
    *out = skin::Resolve(skin, values);
    return true;
}

bool HoldsSpectrum(const skin::Panel &panel) {
    for (const skin::Line &l : panel.lines)
        for (const skin::Text &t : l.texts)
            if (t.spectrum) return true;
    return false;
}

}  // namespace

std::vector<std::wstring> LiveSpectrum::Panels(const theme::Theme &theme) const {
    std::vector<std::wstring> out;
    for (const theme::Instance &i : theme.components) {
        skin::Resolved r;
        if (!i.visible || !ResolveInstance(library_, i, &r)) continue;
        for (size_t k = 0; k < r.panels.size(); ++k)
            if (HoldsSpectrum(r.panels[k])) out.push_back(i.id + L".p" + std::to_wstring(k + 1));
    }
    return out;
}

theme::Theme LiveSpectrum::Without(const theme::Theme &theme) const {
    theme::Theme out = theme;
    for (theme::Instance &i : out.components) {
        skin::Resolved r;
        if (!i.visible || !ResolveInstance(library_, i, &r)) continue;
        for (size_t k = 0; k < r.panels.size(); ++k)
            if (HoldsSpectrum(r.panels[k])) i.sets[L"p" + std::to_wstring(k + 1) + L".opacity"] = L"0";
    }
    return out;
}

bool LiveSpectrum::Frame(const std::wstring &stream, const theme::Theme &theme, const std::wstring &instance,
                         size_t panel, UINT width, UINT height, const data::Snapshot &data, Picture *out, RECT *where) {
    const theme::Instance *found = nullptr;
    for (const theme::Instance &i : theme.components)
        if (i.id == instance && i.visible) found = &i;
    skin::Resolved r;
    if (!found || !panel || !ResolveInstance(library_, *found, &r) || panel > r.panels.size()) return false;
    skin::Panel alone = std::move(r.panels[panel - 1]);
    r.panels.assign(1, std::move(alone));

    const ULONGLONG now = GetTickCount64();
    asked_ = now;
    if (!capture_.Listening()) capture_.Start();  // this person's default output
    if (now - analysed_ >= kAnalyseMs) {
        float rate = 0.0f;
        if (capture_.Latest(&samples_, &rate)) analyzer_.Run(samples_.data(), samples_.size(), rate, now / 1000.0, &heard_);
        else heard_ = {{}, 0.0f, false, now / 1000.0};  // not open yet: silence
        analysed_ = now;
    }

    // Another component, other values or another size start the bands afresh.
    std::wstring signature = found->ref + L"|" + std::to_wstring(panel) + L"|" + std::to_wstring(width) + L"x" +
                             std::to_wstring(height);
    for (const auto &[key, value] : found->sets) signature += L"|" + key + L"=" + value;
    Entry &entry = views_[stream + L"/" + instance + L".p" + std::to_wstring(panel)];
    if (!entry.view) entry.view = std::make_unique<LivePreview>();
    entry.used = now;
    return entry.view->Draw(signature, r, library_.settings().clock, UserFormat(), data, width, height, heard_, out, where);
}

void LiveSpectrum::Idle() {
    const ULONGLONG now = GetTickCount64();
    if (capture_.Listening() && now - asked_ > kListenMs) capture_.Stop();
    for (auto it = views_.begin(); it != views_.end();) {
        if (now - it->second.used > kKeepMs) it = views_.erase(it);
        else ++it;
    }
}

}  // namespace app
