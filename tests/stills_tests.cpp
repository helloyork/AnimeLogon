// The displays other than the primary after a wake: the other_displays setting, which displays
// get a still, what each is cut from, and the blur.
#include "check.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "animelogon/background.h"
#include "animelogon/bitmap.h"
#include "animelogon/components.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"
#include "animelogon/text.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"
#include "picture.h"
#include "plan.h"
#include "stills.h"

using namespace animelogon;

namespace {

const std::wstring kVideo = L"1111111111111111";
const std::wstring kImage = L"3333333333333333";
const std::wstring kThemeImage = L"aaaaaaaaaaaaaaaa";  // the image, filling
const std::wstring kThemeNone = L"bbbbbbbbbbbbbbbb";   // no wallpaper
const std::wstring kThemeVideo = L"cccccccccccccccc";  // the video, fitting

class FakeStore : public ThemeStore {
public:
    std::map<std::wstring, std::string> themes;
    std::map<std::wstring, WallpaperInfo> wallpapers;

    bool LoadTheme(const std::wstring &id, theme::Theme *t, std::wstring *why) const override {
        if (id == kDefaultTheme) return ::animelogon::LoadTheme(id, t, why);
        const auto it = themes.find(id);
        if (it == themes.end()) return *why = L"no such theme", false;
        return theme::Parse(it->second, theme::Form::Installed, t, why);
    }
    bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) const override {
        if (id == kDefaultWallpaper) return ::animelogon::LoadWallpaper(id, info, why);
        const auto it = wallpapers.find(id);
        if (it == wallpapers.end()) return *why = L"no such wallpaper", false;
        *info = it->second;
        return true;
    }
    bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) const override {
        if (id == kClockComponent) return ::animelogon::LoadComponent(id, component, why);
        return *why = L"no such component", false;
    }
};

// The image wallpaper is 8 x 2 pixels, each a colour of its own; it lives at `imagePath`.
FakeStore Store(const std::wstring &imagePath) {
    FakeStore store;
    WallpaperInfo image;
    image.id = kImage;
    image.kind = WallpaperKind::Image;
    image.width = 8;
    image.height = 2;
    image.imagePath = imagePath;
    store.wallpapers[kImage] = image;
    WallpaperInfo video;
    video.id = kVideo;
    video.kind = WallpaperKind::Video;
    video.width = 1920;
    video.height = 1080;
    video.videoPath = L"C:\\ProgramData\\AnimeLogon\\wallpapers\\" + kVideo + L"\\video.mp4";
    store.wallpapers[kVideo] = video;
    store.themes[kThemeImage] = "<theme format=\"1\" name=\"I\"><wallpaper ref=\"3333333333333333\" fit=\"fill\"/></theme>";
    store.themes[kThemeNone] = "<theme format=\"1\" name=\"N\"><wallpaper ref=\"none\" fit=\"fill\"/></theme>";
    store.themes[kThemeVideo] = "<theme format=\"1\" name=\"V\"><wallpaper ref=\"1111111111111111\" fit=\"fit\"/></theme>";
    return store;
}

MonitorInfo Monitor(const wchar_t *key, LONG left, LONG top, LONG right, LONG bottom, bool primary) {
    MonitorInfo m;
    m.key = key;
    m.rect = {left, top, right, bottom};
    m.primary = primary;
    return m;
}

// Three small displays: the primary on the left, one beside it, one below them both.
std::vector<MonitorInfo> Monitors(size_t n = 3) {
    std::vector<MonitorInfo> all = {Monitor(L"00000000000000aa", 0, 0, 4, 2, true),
                                    Monitor(L"00000000000000bb", 4, 0, 8, 2, false),
                                    Monitor(L"00000000000000cc", 0, 2, 8, 4, false)};
    all.resize(n);
    return all;
}

bool Same(const RECT &a, const RECT &b) { return EqualRect(&a, &b) != FALSE; }

std::vector<uint8_t> ImagePixels() {
    std::vector<uint8_t> bgra(8 * 2 * 4);
    for (int i = 0; i < 16; ++i) {
        bgra[i * 4 + 0] = (uint8_t)(10 + i * 15);
        bgra[i * 4 + 1] = (uint8_t)(200 - i * 11);
        bgra[i * 4 + 2] = (uint8_t)(i * 7);
        bgra[i * 4 + 3] = 255;
    }
    return bgra;
}

// The image, in a fresh directory under %TEMP% named in its long form: the image is opened as the
// overlay opens wallpapers, which refuses a path whose final form differs (a short 8.3 name, as
// %TEMP% is on some machines, would read as a link).
std::wstring WriteImage() {
    wchar_t temp[MAX_PATH + 1], longer[32768];
    GetTempPathW(ARRAYSIZE(temp), temp);
    const DWORD n = GetLongPathNameW(temp, longer, ARRAYSIZE(longer));
    std::wstring dir = n && n < ARRAYSIZE(longer) ? longer : temp;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\animelogon-stills-" + RandomHex(6);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::vector<uint8_t> pixels = ImagePixels();
    const std::vector<uint8_t> file = bitmap::Build(8, 2, pixels.data(), pixels.size());
    const std::wstring path = dir + L"\\image.bmp";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, file.data(), (DWORD)file.size(), &wrote, nullptr);
        CloseHandle(h);
    }
    return path;
}

void Remove(const std::wstring &path) {
    DeleteFileW(path.c_str());
    RemoveDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str());
}

// Pixel (x, y) of the image, B G R.
bool IsImagePixel(const uint8_t *bgr, int x, int y) {
    const std::vector<uint8_t> image = ImagePixels();
    const uint8_t *p = &image[((size_t)y * 8 + x) * 4];
    return bgr[0] == p[0] && bgr[1] == p[1] && bgr[2] == p[2];
}

}  // namespace

TEST(OtherDisplaysSettingDefaultsToBlur) {
    const Settings d;
    CHECK(d.otherDisplays == OtherDisplays::Blur);
    CHECK(SerializeSettings(d).find(L"\r\nother_displays = blur\r\n") != std::wstring::npos);
    for (const auto &[text, want] : std::vector<std::pair<std::wstring, OtherDisplays>>{
             {L"blur", OtherDisplays::Blur}, {L"picture", OtherDisplays::Picture}, {L"black", OtherDisplays::Black},
             {L"Black", OtherDisplays::Black}}) {
        std::vector<std::wstring> problems;
        const Settings s = ParseSettings(L"theme = default\r\nother_displays = " + text + L"\r\n", &problems);
        CHECK(problems.empty() && s.otherDisplays == want);
        CHECK(ParseSettings(SerializeSettings(s)).otherDisplays == want);
    }
    // A bad value is reported and keeps the default; the key does not disturb its neighbours.
    std::vector<std::wstring> problems;
    const Settings s = ParseSettings(L"theme = default\r\nother_displays = grey\r\nother_displays =\r\ncomponent_displays = all\r\n", &problems);
    CHECK(problems.size() == 2 && s.otherDisplays == OtherDisplays::Blur && s.componentDisplays == ComponentDisplays::All);
    OtherDisplays out = OtherDisplays::Picture;
    CHECK(!Parse(L"none", &out) && out == OtherDisplays::Picture);
    CHECK(std::wstring(ToString(OtherDisplays::Picture)) == L"picture");
}

TEST(StillsGoToEveryDisplayButThePrimary) {
    const FakeStore store = Store(L"C:\\nowhere\\image.bmp");
    Settings s;
    s.theme = kThemeImage;

    // Duplicate: every other display, each its own canvas, the same wallpaper.
    plan::Plan p = plan::Build(s, Monitors(), store);
    std::vector<stills::Still> chosen = stills::Choose(p, OtherDisplays::Blur);
    CHECK(chosen.size() == 2);
    for (size_t i = 0; i < chosen.size(); ++i) {
        const stills::Still &still = chosen[i];
        CHECK(still.monitorKey == p.displays[i + 1].monitorKey && Same(still.rect, p.displays[i + 1].rect));
        CHECK(Same(still.canvas, still.rect) && still.wallpaper.id == kImage && still.wallpaper.fit == Scaling::Fill);
    }
    CHECK(stills::Choose(p, OtherDisplays::Picture).size() == 2);
    // Black, a single display, a wallpaper of none: no still anywhere.
    CHECK(stills::Choose(p, OtherDisplays::Black).empty());
    CHECK(stills::Choose(plan::Build(s, Monitors(1), store), OtherDisplays::Blur).empty());
    s.theme = kThemeNone;
    CHECK(stills::Choose(plan::Build(s, Monitors(), store), OtherDisplays::Blur).empty());

    // Spanning: the primary's wallpaper across the wall, each still its display's slice of it.
    s.theme = kThemeImage;
    s.monitorMode = MonitorMode::Span;
    p = plan::Build(s, Monitors(), store);
    chosen = stills::Choose(p, OtherDisplays::Blur);
    const RECT wall{0, 0, 8, 4};
    CHECK(chosen.size() == 2);
    for (size_t i = 0; i < chosen.size(); ++i)
        CHECK(Same(chosen[i].canvas, wall) && Same(chosen[i].rect, p.displays[i + 1].rect) && chosen[i].wallpaper.id == kImage);

    // Per-monitor: each its own theme; one showing nothing gets nothing, and the primary never
    // does, whatever it shows.
    s.monitorMode = MonitorMode::PerMonitor;
    s.theme = kThemeImage;
    s.screens[L"00000000000000bb"] = kThemeVideo;
    s.screens[L"00000000000000cc"] = kThemeNone;
    p = plan::Build(s, Monitors(), store);
    chosen = stills::Choose(p, OtherDisplays::Blur);
    CHECK(chosen.size() == 1 && chosen[0].monitorKey == L"00000000000000bb");
    CHECK(chosen.size() == 1 && chosen[0].wallpaper.source == plan::Source::Video && chosen[0].wallpaper.id == kVideo &&
          chosen[0].wallpaper.fit == Scaling::Fit && Same(chosen[0].canvas, chosen[0].rect));
    s.screens[L"00000000000000cc"] = L"default";
    chosen = stills::Choose(plan::Build(s, Monitors(), store), OtherDisplays::Blur);
    CHECK(chosen.size() == 2 && chosen[1].wallpaper.source == plan::Source::Gradient);

    // A plan that names no primary: the first display, where EnumerateMonitors puts it, is taken for it.
    for (plan::Display &d : p.displays) d.primary = false;
    chosen = stills::Choose(p, OtherDisplays::Blur);
    CHECK(chosen.size() == 1 && chosen[0].monitorKey == L"00000000000000bb");
}

TEST(StillsAreCutFromWhatTheDisplayShows) {
    const std::wstring path = WriteImage();
    const FakeStore store = Store(path);
    Settings s;
    s.theme = kThemeImage;
    std::vector<uint8_t> bgr;
    std::wstring why;

    // Spanning two displays side by side, the 8 x 2 image fills the 8 x 2 wall exactly: the
    // display on the right shows the image's right half.
    s.monitorMode = MonitorMode::Span;
    std::vector<stills::Still> chosen = stills::Choose(plan::Build(s, Monitors(2), store), OtherDisplays::Blur);
    CHECK(chosen.size() == 1);
    CHECK(chosen.size() == 1 && stills::Compose(chosen[0], &bgr, &why) && bgr.size() == 4 * 2 * 3);
    bool right = bgr.size() == 4 * 2 * 3;
    for (int y = 0; right && y < 2; ++y)
        for (int x = 0; right && x < 4; ++x) right = IsImagePixel(&bgr[((size_t)y * 4 + x) * 3], 4 + x, y);
    CHECK(right);

    // Duplicating, each display fills itself with the whole image: a 4 x 2 display crops the
    // middle four columns.
    s.monitorMode = MonitorMode::Duplicate;
    chosen = stills::Choose(plan::Build(s, Monitors(2), store), OtherDisplays::Blur);
    CHECK(chosen.size() == 1 && stills::Compose(chosen[0], &bgr, &why) && bgr.size() == 4 * 2 * 3);
    bool middle = bgr.size() == 4 * 2 * 3;
    for (int y = 0; middle && y < 2; ++y)
        for (int x = 0; middle && x < 4; ++x) middle = IsImagePixel(&bgr[((size_t)y * 4 + x) * 3], 2 + x, y);
    CHECK(middle);

    // The built-in wallpaper: the gradient as tall as the canvas, the same in every column. On
    // the display below the others, spanning, it is the lower half of the wall's gradient.
    s.theme = L"default";
    s.monitorMode = MonitorMode::Span;
    chosen = stills::Choose(plan::Build(s, Monitors(3), store), OtherDisplays::Blur);
    CHECK(chosen.size() == 2 && chosen[1].wallpaper.source == plan::Source::Gradient);
    if (chosen.size() == 2) {
        CHECK(stills::Compose(chosen[1], &bgr, &why) && bgr.size() == 8 * 2 * 3);
        const std::vector<uint8_t> column = background::Pixels(1, 4);
        bool lower = bgr.size() == 8 * 2 * 3;
        for (int y = 0; lower && y < 2; ++y)
            for (int x = 0; lower && x < 8; ++x)
                for (int c = 0; c < 3; ++c) lower = lower && bgr[((size_t)y * 8 + x) * 3 + c] == column[(size_t)(y + 2) * 4 + c];
        CHECK(lower);
        CHECK(stills::BuiltIn(chosen[1]) == bgr);
    }

    // A video is made elsewhere (bake::FirstFrameOn); an image that is not there cannot be cut.
    s.theme = kThemeVideo;
    s.monitorMode = MonitorMode::Duplicate;
    chosen = stills::Choose(plan::Build(s, Monitors(2), store), OtherDisplays::Blur);
    why.clear();
    CHECK(chosen.size() == 1 && !stills::Compose(chosen[0], &bgr, &why) && !why.empty());
    Remove(path);
    s.theme = kThemeImage;
    chosen = stills::Choose(plan::Build(s, Monitors(2), store), OtherDisplays::Blur);
    why.clear();
    CHECK(chosen.size() == 1 && !stills::Compose(chosen[0], &bgr, &why) && !why.empty());
}

TEST(SoftenedStillsAreSmoothAndDarker) {
    // As it is: the picture's own bytes, opaque.
    std::vector<uint8_t> bgr = {1, 2, 3, 250, 251, 252};
    std::vector<uint8_t> bgra(8, 7);
    stills::Finish(bgr, 2, 1, OtherDisplays::Picture, bgra.data());
    CHECK((bgra == std::vector<uint8_t>{1, 2, 3, 255, 250, 251, 252, 255}));

    // The tone: grey goes darker and toward a dark grey, white to about 180 as Windows has it,
    // and a colour keeps a little more of its colour than grey does.
    float toned[3];
    const float white[3] = {255, 255, 255}, black[3] = {0, 0, 0}, red[3] = {40, 40, 220};
    stills::Tone(white, toned);
    CHECK(std::abs(toned[0] - 180.5f) < 1.0f && toned[0] == toned[1] && toned[1] == toned[2]);
    stills::Tone(black, toned);
    CHECK(std::abs(toned[0] - stills::kLift) < 0.01f);
    stills::Tone(red, toned);
    CHECK(toned[2] - toned[1] > stills::kLevel * 180.0f && toned[0] == toned[1]);

    // One colour stays that colour, toned, to within the grain, at any size -- including a grid
    // whose last row and column are partly empty.
    for (const auto &[w, h] : std::vector<std::pair<int, int>>{{64, 48}, {1003, 701}}) {
        bgr.assign((size_t)w * h * 3, 0);
        for (size_t i = 0; i < bgr.size(); i += 3) bgr[i] = 100, bgr[i + 1] = 150, bgr[i + 2] = 200;
        const float source[3] = {100, 150, 200};
        stills::Tone(source, toned);
        bgra.assign((size_t)w * h * 4, 0);
        stills::Finish(bgr, w, h, OtherDisplays::Blur, bgra.data());
        bool flat = true;
        double sum = 0;
        for (size_t i = 0; flat && i < bgra.size(); i += 4) {
            for (int c = 0; c < 3; ++c) flat = flat && std::abs(bgra[i + c] - toned[c]) <= stills::kGrain + 1.0f;
            flat = flat && bgra[i + 3] == 255;
            sum += bgra[i + 1];
        }
        CHECK(flat);
        CHECK(std::abs(sum / ((double)w * h) - toned[1]) < 0.5);  // the grain averages out
    }

    // A hard edge becomes a soft one, about kBlurSigma of the height wide, and the same input
    // always gives the same bytes.
    const int w = 256, h = 100;
    bgr.assign((size_t)w * h * 3, 0);
    for (int y = 0; y < h; ++y)
        for (int x = w / 2; x < w; ++x)
            for (int c = 0; c < 3; ++c) bgr[((size_t)y * w + x) * 3 + c] = 255;
    bgra.assign((size_t)w * h * 4, 0);
    stills::Finish(bgr, w, h, OtherDisplays::Blur, bgra.data());
    const float low = stills::kLift, high = 255.0f * stills::kLevel + stills::kLift, span = high - low;
    const float sigma = stills::kBlurSigma * h, slack = stills::kGrain + 1.0f;
    // The share of the way from the dark side to the light one at x, from the middle row.
    auto at = [&](int x) { return ((float)bgra[((size_t)(h / 2) * w + x) * 4 + 1] - low) / span; };
    CHECK(std::abs(at(0)) * span <= slack && std::abs(at(w - 1) - 1.0f) * span <= slack);
    CHECK(std::abs(at(w / 2) - 0.5f) <= 0.15f && std::abs(at(w / 2 - 1) - 0.5f) <= 0.15f);
    CHECK(at(w / 2 + (int)(2.5f * sigma)) > 0.9f && at(w / 2 - (int)(2.5f * sigma)) < 0.1f);
    CHECK(at(w / 2 + (int)(sigma / 2)) < 0.9f && at(w / 2 - (int)(sigma / 2)) > 0.1f);
    std::vector<uint8_t> again(bgra.size(), 0);
    stills::Finish(bgr, w, h, OtherDisplays::Blur, again.data());
    CHECK(again == bgra);
}
