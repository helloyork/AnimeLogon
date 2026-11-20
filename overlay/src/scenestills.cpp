#include "scenestills.h"

#include <vector>

#include "animelogon/log.h"
#include "animelogon/resolve.h"
#include "animelogon/scenehost.h"
#include "animelogon/secure.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

using namespace animelogon;

namespace {

// A still that takes longer than this is given up on: the plan shows still.bmp in its place.
constexpr ULONGLONG kGiveUpMs = 30000;

// The theme a still in StillsDir() belongs to: its name up to the '-' (wallpaper.h SceneStillPath).
std::wstring ThemeOf(const std::wstring &name) {
    const size_t dash = name.find(L'-');
    return dash == std::wstring::npos ? std::wstring() : name.substr(0, dash);
}

// Deletes the stills in StillsDir() that `drop` picks by file name.
template <class Drop>
void Delete(const Drop &drop) {
    const std::wstring dir = StillsDir();
    WIN32_FIND_DATAW f{};
    const HANDLE h = FindFirstFileW((dir + L"\\*.bmp").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> gone;
    do {
        if (f.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (drop(std::wstring(f.cFileName))) gone.push_back(dir + L"\\" + f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    for (const std::wstring &path : gone) DeleteFileW(path.c_str());
}

}  // namespace

const plan::Wallpaper *StillToDraw(const plan::Plan &plan, const std::set<std::wstring> &failed) {
    for (const plan::Display &d : plan.displays) {
        const plan::Wallpaper &w = d.wallpaper;
        if (w.source == plan::Source::Scene && !w.ownStill.empty() && w.path != w.ownStill && !failed.count(w.ownStill))
            return &w;
    }
    return nullptr;
}

SceneStills::SceneStills() = default;
SceneStills::~SceneStills() = default;

bool SceneStills::Want(const plan::Plan &plan, ID3D11Device *device) {
    if (!pruned_) {
        pruned_ = true;
        // The stills of themes that have been removed since.
        Delete([](const std::wstring &name) {
            const std::wstring id = ThemeOf(name);
            if (id == kDefaultTheme) return false;
            theme::Theme t;
            std::wstring why;
            return !IsThemeId(id) || !DiskStore().LoadTheme(id, &t, &why);
        });
    }
    const plan::Wallpaper *w = StillToDraw(plan, failed_);
    if (!w) {
        // The values changed again while it was drawn, or the theme is no longer shown.
        host_.reset();
        return false;
    }
    if (host_ && path_ == w->ownStill) return true;
    host_.reset();
    path_ = w->ownStill;
    id_ = w->id;
    // Without a device now, the bake goes ahead with still.bmp; the still is drawn next time.
    if (!device) return false;
    scene::Files files;
    scenegraph::Document document;
    std::wstring why;
    if (w->sceneWidth <= 0 || w->sceneHeight <= 0) {
        Fail(L"the scene has no size");
        return false;
    }
    if (!scene::LoadWithValues(w->sceneDir, w->values, &files, &document, &why)) {
        Fail(why);
        return false;
    }
    // As the import draws still.bmp: the moment the scene names, still, at the highest quality.
    scene::Options options;
    options.tier = {true, 0, 1};
    options.quality = RenderQuality::Highest;
    options.initial.data.time = document.still;
    auto host = std::make_unique<scene::Host>();
    if (!host->Start(device, files, w->sceneWidth, w->sceneHeight, options, &why)) {
        Fail(why);
        return false;
    }
    host_ = std::move(host);
    started_ = GetTickCount64();
    return true;
}

bool SceneStills::Tick(bool allowed) {
    if (!host_) return false;
    if (!allowed) {
        host_.reset();
        ALOG(L"scene: the still of %s with its theme's settings waits for the desktop", id_.c_str());
        return false;
    }
    host_->Poll();
    std::wstring why;
    if (host_->Failed(&why)) {
        Fail(why);
        return false;
    }
    if (!host_->Ready()) {
        if (GetTickCount64() - started_ > kGiveUpMs) Fail(L"no picture in 30 seconds");
        return false;
    }
    const std::vector<uint8_t> bitmap = host_->Bitmap(&why);
    if (bitmap.empty()) {
        Fail(why);
        return false;
    }
    const std::wstring dir = StillsDir();
    DWORD e = secure::IsTrustedDirectory(dir, &why) ? ERROR_SUCCESS : secure::SecureDirectory(dir);
    if (e == ERROR_SUCCESS) e = secure::WriteBytes(path_, bitmap.data(), bitmap.size());
    if (e != ERROR_SUCCESS) {
        Fail(Format(L"it could not be written (%lu)", e));
        return false;
    }
    // A theme keeps one still: the one for its values now.
    const std::wstring name = path_.substr(path_.find_last_of(L'\\') + 1), theme = ThemeOf(name);
    Delete([&](const std::wstring &other) { return ThemeOf(other) == theme && _wcsicmp(other.c_str(), name.c_str()) != 0; });
    ALOG(L"scene: %s drawn with its theme's settings as %s in %llu ms", id_.c_str(), name.c_str(),
         GetTickCount64() - started_);
    host_.reset();
    return true;
}

void SceneStills::Fail(const std::wstring &why) {
    host_.reset();
    failed_.insert(path_);
    ALOG(L"scene: the still of %s with its theme's settings could not be drawn (%s); its own still.bmp stands in",
         id_.c_str(), why.c_str());
}
