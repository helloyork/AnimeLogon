// The settings app's pictures of themes (app/src/pictures.h, wall.h): the wall of displays when
// spanning, and the components alone as a layer over a video the page plays. Drawn by the logon
// screen's own renderer, on WARP where there is no graphics card.
#include "check.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "animelogon/clock.h"
#include "animelogon/components.h"
#include "animelogon/monitors.h"
#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "animelogon/skinpreview.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

#include "library.h"
#include "pictures.h"
#include "wall.h"

using Microsoft::WRL::ComPtr;
using namespace animelogon;

namespace {

const std::wstring kTheme = L"cccccccccccccccc";

// One theme: the built-in wallpaper and the clock at the bottom left.
class OneThemeStore : public ThemeStore {
public:
    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        if (id == kDefaultTheme) return *t = theme::Default(), true;
        if (id != kTheme) return false;
        return theme::Parse("<theme format=\"1\" name=\"W\"><wallpaper ref=\"default\" fit=\"fill\"/>"
                            "<component id=\"clock\" ref=\"clock\"><set key=\"p1.anchor\" value=\"bottom-left\"/>"
                            "</component></theme>",
                            theme::Form::Installed, t, why);
    }
    bool LoadWallpaper(const std::wstring &, WallpaperInfo *, std::wstring *) const override { return false; }
    bool LoadComponent(const std::wstring &id, skin::Skin *s, std::wstring *) const override {
        if (id != kClockComponent) return false;
        *s = skin::Default();
        return true;
    }
};

class OneThemeStores : public app::Stores {
public:
    OneThemeStore store;
    std::vector<WallpaperInfo> wallpapers;
    const ThemeStore &Store() const override { return store; }
    std::vector<ThemeEntry> Themes() const override {
        return {{kDefaultTheme, L"默认", L"AnimeLogon", true}, {kTheme, L"W", L"", false}};
    }
    std::vector<WallpaperInfo> Wallpapers() const override { return wallpapers; }
    std::vector<ComponentEntry> Components() const override { return {{kClockComponent, L"时钟", L"AnimeLogon", true}}; }
    Settings ReadSettings() const override { return ParseSettings(L"theme = " + kTheme + L"\r\n"); }
    bool WriteSettings(const std::wstring &) const override { return true; }
};

MonitorInfo Monitor(const wchar_t *key, RECT rect, bool primary) {
    MonitorInfo m;
    m.key = key;
    m.rect = rect;
    m.primary = primary;
    return m;
}

// A 1920 x 1080 main display, and a 1280 x 1024 one to its right, 56 pixels lower.
std::vector<MonitorInfo> TwoDisplays() {
    return {Monitor(L"main", {0, 0, 1920, 1080}, true), Monitor(L"side", {1920, 56, 3200, 1080}, false)};
}

bool Within(const D2D1_RECT_F &r, const RECT &screen) {
    return r.left >= screen.left - 0.5f && r.top >= screen.top - 0.5f && r.right <= screen.right + 0.5f &&
           r.bottom <= screen.bottom + 0.5f && r.right > r.left && r.bottom > r.top;
}

// A second of one colour as H.264, 320 x 180: not whole macroblocks, so the decoded frame is
// taller than the picture. False where Media Foundation cannot encode H.264.
bool SolidClip(const std::wstring &path) {
    constexpr UINT32 w = 320, h = 180, fps = 10;
    auto type = [&](const GUID &subtype) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, subtype);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (subtype == MFVideoFormat_H264) t->SetUINT32(MF_MT_AVG_BITRATE, 500000);
        return t;
    };
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    if (FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer)) ||
        FAILED(writer->AddStream(type(MFVideoFormat_H264).Get(), &stream)) ||
        FAILED(writer->SetInputMediaType(stream, type(MFVideoFormat_NV12).Get(), nullptr)) || FAILED(writer->BeginWriting()))
        return false;
    // Y 150, U 60, V 200: a strong orange.
    std::vector<uint8_t> nv12(w * h * 3 / 2, 150);
    for (size_t i = w * h; i < nv12.size(); i += 2) nv12[i] = 60, nv12[i + 1] = 200;
    for (UINT32 f = 0; f < fps; ++f) {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE *p = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)nv12.size(), &buffer)) || FAILED(buffer->Lock(&p, nullptr, nullptr))) return false;
        memcpy(p, nv12.data(), nv12.size());
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD)nv12.size());
        if (FAILED(MFCreateSample(&sample))) return false;
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime((LONGLONG)f * 10000000 / fps);
        sample->SetSampleDuration(10000000 / fps);
        if (FAILED(writer->WriteSample(stream, sample.Get()))) return false;
    }
    return SUCCEEDED(writer->Finalize());
}

}  // namespace

TEST(AppWallFollowsTheDisplays) {
    const std::vector<MonitorInfo> two = TwoDisplays();
    Settings s;
    s.monitorMode = MonitorMode::Span;
    // The wall is 3200 x 1080: a picture 640 wide is 216 tall.
    CHECK(app::PictureHeight(s, two, &two[0], 640) == 216);
    std::vector<app::Screen> wall = app::Wall(s, two, 640, 216);
    CHECK(wall.size() == 2);
    CHECK(wall[0].rect.left == 0 && wall[0].rect.top == 0 && wall[0].rect.right == 384 && wall[0].rect.bottom == 216);
    CHECK(wall[1].rect.left == 384 && wall[1].rect.top == 11 && wall[1].rect.right == 640 && wall[1].rect.bottom == 216);
    // Components start on the main display, unless they are shown on every one.
    CHECK(wall[0].components && !wall[1].components);
    s.componentDisplays = ComponentDisplays::All;
    wall = app::Wall(s, two, 640, 216);
    CHECK(wall[0].components && wall[1].components);

    // Not spanning, or one display: the picture is the display it is shown on.
    s.monitorMode = MonitorMode::Duplicate;
    CHECK(app::Wall(s, two, 640, 216).empty() && app::PictureHeight(s, two, &two[1], 640) == 512);
    s.monitorMode = MonitorMode::Span;
    const std::vector<MonitorInfo> one{two[0]};
    CHECK(app::Wall(s, one, 640, 360).empty() && app::PictureHeight(s, one, &one[0], 640) == 360);
    CHECK(app::PictureHeight(s, {}, nullptr, 160) == 90);
}

TEST(AppPicturesDrawComponentsOnEachScreenOfTheWall) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OneThemeStores stores;
    app::Library library(stores);
    app::Pictures pictures(library);
    const theme::Theme t = library.Edited(kTheme);

    Settings s;
    s.monitorMode = MonitorMode::Span;
    s.componentDisplays = ComponentDisplays::All;
    const std::vector<app::Screen> wall = app::Wall(s, TwoDisplays(), 640, 216);
    Picture p;
    std::vector<app::Bound> bounds;
    CHECK(pictures.Theme(t, 640, 216, app::Draw::Everything, &p, &bounds, &wall));
    CHECK(p.width == 640 && p.height == 216 && p.bgra.size() == 640u * 216 * 4);
    bool on[2] = {false, false};
    for (const app::Bound &b : bounds) {
        CHECK(b.instance == L"clock" && b.screen < 2);
        if (b.screen >= 2) continue;
        on[b.screen] = true;
        CHECK(Within(b.rect, wall[b.screen].rect));
    }
    CHECK(on[0] && on[1]);

    // Only the main display shows them.
    std::vector<app::Screen> mainOnly = wall;
    mainOnly[1].components = false;
    bounds.clear();
    CHECK(pictures.Theme(t, 640, 216, app::Draw::Everything, &p, &bounds, &mainOnly));
    CHECK(!bounds.empty());
    for (const app::Bound &b : bounds) CHECK(b.screen == 0 && Within(b.rect, mainOnly[0].rect));
    if (SUCCEEDED(co)) CoUninitialize();
}

TEST(AppPicturesShowAVideoWallpapersFirstFrame) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    wchar_t temp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"animelogon-solid-" + std::to_wstring(GetCurrentProcessId()) + L".mp4";
    if (!mf || !SolidClip(path)) {
        std::printf("  skipped: Media Foundation cannot encode H.264 here\n");
    } else {
        OneThemeStores stores;
        WallpaperInfo w;
        w.id = L"dddddddddddddddd";
        w.kind = WallpaperKind::Video;
        w.videoPath = path;
        w.width = 320;
        w.height = 180;
        stores.wallpapers.push_back(w);
        app::Library library(stores);
        app::Pictures pictures(library);
        Picture p;
        CHECK(pictures.Wallpaper(w.id, Scaling::Fill, 64, 36, &p) && p.width == 64 && p.height == 36);
        // Orange, not the dark built-in wallpaper shown in place of one that cannot be read; down to
        // the last row, which comes from the picture and not from the padding the decoder adds.
        const uint8_t *mid = &p.bgra[((size_t)18 * 64 + 32) * 4], *low = &p.bgra[((size_t)35 * 64 + 32) * 4];
        CHECK(mid[2] > 180 && mid[0] < 90);
        CHECK(low[2] > 180 && low[0] < 90);
    }
    DeleteFileW(path.c_str());
    if (mf) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

TEST(AppPicturesLayerIsTheComponentsAlone) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OneThemeStores stores;
    app::Library library(stores);
    app::Pictures pictures(library);
    Picture layer;
    std::vector<app::Bound> bounds;
    CHECK(pictures.Theme(library.Edited(kTheme), 320, 180, app::Draw::ComponentsOnly, &layer, &bounds));
    CHECK(layer.width == 320 && layer.height == 180 && !bounds.empty());
    // Nothing at the top right, far from the clock at the bottom left; something where it is.
    auto alpha = [&](UINT x, UINT y) { return layer.bgra[((size_t)y * layer.width + x) * 4 + 3]; };
    CHECK(alpha(315, 4) == 0 && alpha(200, 20) == 0);
    unsigned covered = 0;
    for (const app::Bound &b : bounds)
        if (b.key == L"p1")
            for (UINT y = (UINT)b.rect.top; y < (UINT)b.rect.bottom && y < layer.height; ++y)
                for (UINT x = (UINT)b.rect.left; x < (UINT)b.rect.right && x < layer.width; ++x) covered += alpha(x, y) > 0;
    CHECK(covered > 0);
    // Premultiplied: no colour brighter than its coverage.
    bool premultiplied = true;
    for (size_t i = 0; i < layer.bgra.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) premultiplied = premultiplied && layer.bgra[i + c] <= layer.bgra[i + 3];
    CHECK(premultiplied);
    const std::vector<uint8_t> png = app::Png(layer);
    CHECK(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    if (SUCCEEDED(co)) CoUninitialize();
}

// A panel says which of its points is held at its anchor. A centred 12-hour clock is held by the
// middle of its figures, with the AM/PM marker hanging to their right, and by the baseline of
// its last line at the bottom; the settings app snaps a dragged panel by these points.
TEST(SkinPanelsSayThePointTheyAreHeldBy) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SkinPreview preview;
    Picture background;
    background.width = 640;
    background.height = 360;
    background.bgra.assign((size_t)640 * 360 * 4, 0);
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    SYSTEMTIME now{};
    now.wYear = 2026;
    now.wMonth = 10;
    now.wDay = 16;
    now.wDayOfWeek = 5;
    now.wHour = 15;
    now.wMinute = 4;
    const float margin = 0.07f * 360;
    auto about = [](float a, float b) { return std::fabs(a - b) <= 1.5f; };
    for (const wchar_t *anchor : {L"top", L"bottom-right", L"left"}) {
        Picture out;
        std::vector<SkinView::Bound> bounds;
        CHECK(preview.Render(background, skin::Resolve(skin::Default(), {{L"p1.anchor", anchor}}), ClockStyle{}, us, now,
                             &out, &bounds));
        const auto p1 = std::find_if(bounds.begin(), bounds.end(), [](const SkinView::Bound &b) { return b.key == L"p1"; });
        CHECK(p1 != bounds.end());
        if (p1 == bounds.end()) continue;
        const SkinView::Bound &b = *p1;
        CHECK(b.holds);
        if (std::wstring(anchor) == L"top") {
            CHECK(about(b.holdX[1], 320.0f) && about(b.holdY[0], margin));
            CHECK((b.rect.left + b.rect.right) / 2 > b.holdX[1] + 2);  // the marker hangs to the right
        } else if (std::wstring(anchor) == L"bottom-right") {
            CHECK(about(b.holdX[2], 640 - margin) && about(b.holdY[2], 360 - margin));
            CHECK(b.rect.right > b.holdX[2] + 2 && b.rect.bottom > b.holdY[2]);
        } else {
            CHECK(about(b.holdX[0], margin) && about(b.holdY[1], 180.0f) && b.holdX[0] == b.rect.left);
        }
        // The texts are not placed by an anchor.
        for (const SkinView::Bound &t : bounds) CHECK(t.key == L"p1" || !t.holds);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}
