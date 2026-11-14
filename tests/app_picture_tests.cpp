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
    // Counted in, the marker moves the whole first line onto the centre: the panel is held by the
    // middle of its full width, and the date is centred under the time and the marker together.
    {
        Picture out;
        std::vector<SkinView::Bound> bounds;
        CHECK(preview.Render(background, skin::Resolve(skin::Default(), {{L"p1.anchor", L"top"}, {L"p1.l1.t2.hang", L"false"}}),
                             ClockStyle{}, us, now, &out, &bounds));
        const auto at = [&](const wchar_t *key) {
            return std::find_if(bounds.begin(), bounds.end(), [&](const SkinView::Bound &b) { return b.key == key; });
        };
        const auto p1 = at(L"p1"), time = at(L"p1.l1.t1"), marker = at(L"p1.l1.t2"), date = at(L"p1.l2.t1");
        CHECK(p1 != bounds.end() && time != bounds.end() && marker != bounds.end() && date != bounds.end());
        if (p1 != bounds.end() && time != bounds.end() && marker != bounds.end() && date != bounds.end()) {
            CHECK(about(p1->holdX[1], 320.0f) && about(p1->holdX[1], (p1->rect.left + p1->rect.right) / 2));
            CHECK(about((time->rect.left + marker->rect.right) / 2, (date->rect.left + date->rect.right) / 2));
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// What is hidden takes no room, and the space before a text goes only where something is drawn
// before it; a text with a widest width is cut short; a text that rises is drawn higher; a plate
// fills the padding around the ink; a panel at no opacity draws nothing.
TEST(SkinPanelsLeaveOutTrimShiftAndPlate) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SkinPreview preview;
    Picture background;
    background.width = 640;
    background.height = 360;
    background.bgra.assign((size_t)640 * 360 * 4, 0);
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    SYSTEMTIME now{};
    now.wYear = 2026;
    now.wMonth = 11;
    now.wDay = 13;
    now.wHour = 9;
    now.wMinute = 41;
    skin::Skin s;
    std::wstring error;
    CHECK(skin::Parse("<component format=\"1\" name=\"t\"><panel anchor=\"top-left\" size=\"10\" margin=\"20\">"
                      "<line><text value=\"MM\"/><text value=\"MM\" space=\"1\"/></line>"
                      "<backdrop opacity=\"0\" shape=\"plate\" padding-x=\"1\" padding-y=\"0.5\" color=\"#FF0000\"/>"
                      "</panel></component>",
                      &s, &error));
    const float em = 36.0f, margin = 72.0f;
    auto draw = [&](skin::Values values, Picture *out) {
        std::vector<SkinView::Bound> bounds;
        CHECK(preview.Render(background, skin::Resolve(s, values), ClockStyle{}, us, now, out, &bounds));
        std::map<std::wstring, D2D1_RECT_F> at;
        for (const SkinView::Bound &b : bounds) at[b.key] = b.rect;
        return at;
    };
    Picture out;
    auto both = draw({}, &out);
    CHECK(both.count(L"p1.l1.t1") && both.count(L"p1.l1.t2"));
    // Hidden, the first takes no room, and the second starts where the first did, without its space.
    auto alone = draw({{L"p1.l1.t1.show", L"false"}}, &out);
    CHECK(!alone.count(L"p1.l1.t1") && alone.count(L"p1.l1.t2"));
    if (both.count(L"p1.l1.t1") && alone.count(L"p1.l1.t2")) {
        CHECK(std::fabs(alone[L"p1.l1.t2"].left - both[L"p1.l1.t1"].left) <= 1.5f);
        CHECK(alone[L"p1"].right - alone[L"p1"].left < both[L"p1"].right - both[L"p1"].left - em);
    }
    // Hidden altogether: nothing is drawn.
    auto none = draw({{L"p1.l1.t1.show", L"false"}, {L"p1.l1.t2.show", L"false"}}, &out);
    CHECK(!none.count(L"p1"));
    // Cut short: no wider than its widest, an ellipsis included.
    auto cut = draw({{L"p1.l1.t1.max", L"0.8"}}, &out);
    if (cut.count(L"p1.l1.t1")) CHECK(cut[L"p1.l1.t1"].right - cut[L"p1.l1.t1"].left <= 0.8f * em + 1.0f);
    // Raised by half an em: the panel's ink starts at the raised text, so the other one is drawn
    // half an em lower than when neither rises.
    auto inkTop = [&](const D2D1_RECT_F &r) {
        for (int y = 0; y < (int)out.height; ++y)
            for (int x = std::max(0, (int)r.left); x < std::min((int)out.width, (int)r.right); ++x)
                if (out.bgra[((size_t)y * out.width + x) * 4 + 1] > 128) return (float)y;
        return -1.0f;
    };
    both = draw({}, &out);
    const float level = both.count(L"p1.l1.t1") ? inkTop(both[L"p1.l1.t1"]) : -1.0f;
    auto raised = draw({{L"p1.l1.t2.rise", L"0.5"}}, &out);
    if (raised.count(L"p1.l1.t1") && level >= 0.0f)
        CHECK(std::fabs(inkTop(raised[L"p1.l1.t1"]) - level - 0.5f * em) <= 2.0f);
    // A plate: red in its padding, left of the ink and above it, and nothing beyond it.
    draw({{L"p1.b.opacity", L"1"}}, &out);
    auto red = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= (int)out.width || y >= (int)out.height) return false;
        const uint8_t *px = out.bgra.data() + ((size_t)y * out.width + x) * 4;
        return px[2] > 200 && px[1] < 40 && px[0] < 40;
    };
    if (both.count(L"p1")) {
        const D2D1_RECT_F &ink = both[L"p1"];
        CHECK(red((int)(ink.left - 0.5f * em), (int)((ink.top + ink.bottom) / 2)));
        CHECK(red((int)((ink.left + ink.right) / 2), (int)(ink.top - 0.25f * em)));
        CHECK(!red((int)(ink.left - 1.2f * em), (int)((ink.top + ink.bottom) / 2)));
        CHECK(ink.left >= margin - 2.0f);
    }
    // At no opacity, the panel leaves the picture as it was.
    draw({{L"p1.opacity", L"0"}, {L"p1.b.opacity", L"1"}}, &out);
    CHECK(std::all_of(out.bgra.begin(), out.bgra.end(), [i = 0](uint8_t v) mutable { return (i++ % 4) == 3 || v == 0; }));
    if (SUCCEEDED(co)) CoUninitialize();
}

TEST(SkinPanelsStayOnTheirDisplay) {
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
    auto place = [&](std::map<std::wstring, std::wstring> values) {
        Picture out;
        std::vector<SkinView::Bound> bounds;
        CHECK(preview.Render(background, skin::Resolve(skin::Default(), values), ClockStyle{}, us, now, &out, &bounds));
        const auto p1 = std::find_if(bounds.begin(), bounds.end(), [](const SkinView::Bound &b) { return b.key == L"p1"; });
        CHECK(p1 != bounds.end());
        return p1 == bounds.end() ? SkinView::Bound{} : *p1;
    };
    // Offsets that would carry the clock past each edge stop it at that edge.
    const SkinView::Bound right = place({{L"p1.anchor", L"top-right"}, {L"p1.offset-x", L"50"}});
    CHECK(right.rect.right <= 640.0f && right.rect.right >= 638.0f && right.rect.left > 0.0f);
    const SkinView::Bound corner = place({{L"p1.anchor", L"bottom-left"}, {L"p1.offset-x", L"-50"}, {L"p1.offset-y", L"50"}});
    CHECK(corner.rect.left >= 0.0f && corner.rect.left <= 2.0f && corner.rect.bottom <= 360.0f && corner.rect.bottom >= 358.0f);
    const SkinView::Bound up = place({{L"p1.anchor", L"top"}, {L"p1.offset-y", L"-50"}});
    CHECK(up.rect.top >= 0.0f && up.rect.top <= 2.0f);
    // The point it is held by moves with it, so the settings page's marks follow.
    CHECK(right.holdX[2] <= 640.0f && corner.holdY[2] <= 360.0f);
    // An offset that keeps it on the display is left alone.
    const SkinView::Bound kept = place({{L"p1.anchor", L"top"}, {L"p1.offset-y", L"10"}});
    CHECK(std::fabs(kept.holdY[0] - (0.07f * 360 + 36.0f)) <= 1.5f);
    if (SUCCEEDED(co)) CoUninitialize();
}

// The wallpaper is drawn at the theme's brightness, as the logon screen draws it, and the
// components over it are not darkened.
TEST(AppPicturesDrawTheWallpaperAtTheThemesBrightness) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    OneThemeStores stores;
    app::Library library(stores);
    app::Pictures pictures(library);
    theme::Theme t = library.Edited(kTheme);
    Picture full, dim, everything;
    CHECK(pictures.Theme(t, 320, 180, app::Draw::WallpaperOnly, &full));
    t.brightness = 0.5f;
    CHECK(pictures.Theme(t, 320, 180, app::Draw::WallpaperOnly, &dim));
    bool half = full.bgra.size() == dim.bgra.size() && !full.bgra.empty();
    for (size_t i = 0; half && i < full.bgra.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) half = half && dim.bgra[i + c] == (uint8_t)(full.bgra[i + c] * 0.5f + 0.5f);
    CHECK(half);
    std::vector<app::Bound> bounds;
    CHECK(pictures.Theme(t, 320, 180, app::Draw::Everything, &everything, &bounds));
    uint8_t brightest = 0;
    for (const app::Bound &b : bounds)
        if (b.key == L"p1")
            for (UINT y = (UINT)std::max(0.0f, b.rect.top); y < (UINT)b.rect.bottom && y < everything.height; ++y)
                for (UINT x = (UINT)std::max(0.0f, b.rect.left); x < (UINT)b.rect.right && x < everything.width; ++x)
                    brightest = std::max(brightest, everything.bgra[((size_t)y * everything.width + x) * 4 + 2]);
    CHECK(brightest > 200);
    if (SUCCEEDED(co)) CoUninitialize();
}
