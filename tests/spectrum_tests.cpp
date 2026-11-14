// The audio spectrum: reading bands out of the sound (spectrum.h), the <spectrum> element of the
// component format and the built-in component that uses it, and drawing it.
#include "check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "animelogon/components.h"
#include "animelogon/skin.h"
#include "animelogon/skinpreview.h"
#include "animelogon/spectrum.h"
#include "animelogon/text.h"
#include "animelogon/audiocapture.h"

using namespace animelogon;

namespace {

constexpr float kRate = 48000.0f;

// `seconds` of a sine wave at `hz`, at `amplitude` of full scale.
std::vector<float> Sine(float hz, float amplitude, double seconds = 0.1) {
    std::vector<float> out((size_t)(kRate * seconds));
    for (size_t i = 0; i < out.size(); ++i) out[i] = amplitude * (float)std::sin(2.0 * 3.14159265358979 * hz * i / kRate);
    return out;
}

spectrum::Frame FrameOf(const std::vector<float> &samples, double seconds) {
    spectrum::Analyzer analyzer;
    spectrum::Frame frame;
    analyzer.Run(samples.data(), samples.size(), kRate, seconds, &frame);
    return frame;
}

const wchar_t *const kMinimal = LR"(<component format="1" name="t"><panel><line>%s</line></panel></component>)";

bool ParsesWith(const std::wstring &inLine) {
    std::wstring text(kMinimal);
    text.replace(text.find(L"%s"), 2, inLine);
    skin::Skin s;
    return skin::Parse(ToUtf8(text), &s, nullptr);
}

}  // namespace

TEST(SpectrumFindsASineInItsBand) {
    const spectrum::Frame frame = FrameOf(Sine(1000.0f, 0.5f), 0.0);
    CHECK(frame.sound);
    CHECK(std::fabs(frame.binHz - kRate / 4096.0f) < 0.01f);
    // A sine of amplitude a adds up to a*a across its bins.
    float total = 0.0f;
    for (float p : frame.power) total += p;
    CHECK(std::fabs(total - 0.25f) < 0.02f);
    std::vector<float> levels;
    spectrum::Targets(frame, {32, 40.0f, 16000.0f, 1.0f, 0.5f}, &levels);
    CHECK(levels.size() == 32);
    // 1 kHz is log2(1000/40) / log2(16000/40) of the way along: band 17 of 32.
    const size_t loudest = (size_t)(std::max_element(levels.begin(), levels.end()) - levels.begin());
    CHECK(loudest == 17);
    CHECK(levels[loudest] == 1.0f);
    // Two octaves away there is nothing to speak of.
    CHECK(levels[6] < 0.1f && levels[28] < 0.1f);
}

TEST(SpectrumIsAtRestInSilence) {
    const spectrum::Frame quiet = FrameOf(std::vector<float>(4096, 0.0f), 0.0);
    CHECK(!quiet.sound);
    std::vector<float> levels;
    spectrum::Targets(quiet, {}, &levels);
    CHECK(levels.size() == 64 && *std::max_element(levels.begin(), levels.end()) == 0.0f);
    // An empty frame -- no device yet -- is silence too.
    spectrum::Targets(spectrum::Frame{}, {}, &levels);
    CHECK(levels.size() == 64 && *std::max_element(levels.begin(), levels.end()) == 0.0f);
    spectrum::Bands bands;
    for (int i = 0; i < 10; ++i) bands.Follow({{}, 0.0f, false, i / 60.0}, {});
    CHECK(!bands.Moving() && bands.presence() == 0.0f);
}

TEST(SpectrumRisesQuicklyAndFallsBackSlowly) {
    const std::vector<float> tone = Sine(500.0f, 0.5f);
    const spectrum::Shape shape{16, 40.0f, 16000.0f, 1.0f, 0.5f};
    std::vector<float> target;
    spectrum::Targets(FrameOf(tone, 0.0), shape, &target);
    const size_t band = (size_t)(std::max_element(target.begin(), target.end()) - target.begin());
    spectrum::Bands bands;
    double t = 0.0;
    for (; t < 0.15; t += 1.0 / 60.0) bands.Follow(FrameOf(tone, t), shape);
    CHECK(bands.levels()[band] > 0.9f * target[band]);
    CHECK(bands.presence() > 0.3f && bands.Moving());
    // Silence: a tenth of a second later the band is still well up, and its peak above it.
    for (const double end = t + 0.1; t < end; t += 1.0 / 60.0) bands.Follow({{}, 0.0f, false, t}, shape);
    CHECK(bands.levels()[band] > 0.3f);
    CHECK(bands.peaks()[band] > bands.levels()[band]);
    // Some seconds on, everything is at rest and nothing has to be drawn.
    for (const double end = t + 5.0; t < end; t += 1.0 / 60.0) bands.Follow({{}, 0.0f, false, t}, shape);
    CHECK(bands.levels()[band] == 0.0f && bands.peaks()[band] == 0.0f && bands.presence() == 0.0f);
    CHECK(!bands.Moving());
}

TEST(SpectrumSensitivityRaisesTheBands) {
    const spectrum::Frame frame = FrameOf(Sine(2000.0f, 0.02f), 0.0);
    std::vector<float> plain, keen;
    spectrum::Targets(frame, {16, 40.0f, 16000.0f, 1.0f, 0.5f}, &plain);
    spectrum::Targets(frame, {16, 40.0f, 16000.0f, 3.0f, 0.5f}, &keen);
    const size_t band = (size_t)(std::max_element(plain.begin(), plain.end()) - plain.begin());
    CHECK(plain[band] > 0.0f && plain[band] < 1.0f && keen[band] > plain[band]);
    // The sample previews show is like music: every band up some way, none of them flat.
    std::vector<float> sample;
    spectrum::Targets(spectrum::Sample(), {64, 40.0f, 16000.0f, 1.0f, 0.5f}, &sample);
    const float lowest = *std::min_element(sample.begin(), sample.end());
    const float highest = *std::max_element(sample.begin(), sample.end());
    CHECK(lowest > 0.2f && highest - lowest > 0.2f);
}

TEST(SpectrumElementIsPartOfTheFormat) {
    CHECK(ParsesWith(L"<spectrum/>"));
    CHECK(ParsesWith(L"<spectrum bands=\"32\" mode=\"area\" direction=\"both\" bass=\"middle\" gradient=\"vertical\" "
                     L"colors=\"#FF0000 0%, #00FF00 40.5%, #0000FF80 100%\" flow=\"4\" peak=\"0.5\" low=\"60\" high=\"12000\"/>"));
    CHECK(ParsesWith(L"<spectrum gradient=\"level\" colors=\"#FFFFFF 0%\"/>"));
    CHECK(!ParsesWith(L"<spectrum colors=\"#FF0000\"/>"));  // a colour needs its place
    CHECK(!ParsesWith(L"<spectrum gradient=\"none\"/>"));
    CHECK(ParsesWith(L"<text value=\"{time}\"/><spectrum space=\"0.5\" when=\"media.state=playing\"/>"));
    CHECK(!ParsesWith(L"<spectrum value=\"{time}\"/>"));  // it names no data
    CHECK(!ParsesWith(L"<spectrum bands=\"3\"/>"));
    CHECK(!ParsesWith(L"<spectrum bands=\"32.5\"/>"));
    CHECK(!ParsesWith(L"<spectrum mode=\"dots\"/>"));
    CHECK(!ParsesWith(L"<spectrum><text value=\"x\"/></spectrum>"));
}

TEST(SpectrumComponentComesBuiltIn) {
    skin::Skin s;
    CHECK(IsBuiltInComponent(L"spectrum") && BuiltInComponent(L"spectrum", &s));
    CHECK(skin::SourcesOf(s) == std::vector<data::Source>{data::Source::Audio});
    CHECK(std::wstring(data::LabelOf(data::Source::Audio)) == L"音频");
    // A wide band of bars at the bottom of the screen, coloured left to right.
    skin::Resolved r = skin::Resolve(s, {});
    CHECK(r.panels.size() == 1 && r.panels[0].anchor == 7);
    CHECK(skin::Listens(r) && !skin::Listens(skin::Resolve(skin::Default(), {})));
    const skin::Text &t = r.panels[0].lines[0].texts[0];
    CHECK(t.spectrum && t.name == L"a1" && t.length == 13.0f && t.audio.bands == 64);
    CHECK(t.audio.mode == skin::Mode::Bars && t.audio.gradient == skin::Gradient::Horizontal && t.audio.idleOpacity == 1.0f);
    // Its settings, and adjustments element by element.
    r = skin::Resolve(s, {{L"bands", L"32"}, {L"mode", L"line"}, {L"hide", L"on"}, {L"p1.l1.a1.bass", L"middle"},
                          {L"p1.l1.a1.peak", L"0.8"}, {L"sensitivity", L"2.5"}});
    const skin::Text &u = r.panels[0].lines[0].texts[0];
    CHECK(u.audio.bands == 32 && u.audio.mode == skin::Mode::Line && u.audio.idleOpacity == 0.0f);
    CHECK(u.audio.bass == skin::Bass::Middle && u.audio.peak == 0.8f && u.audio.sensitivity == 2.5f);
    CHECK(skin::Accepts(s, L"p1.l1.a1.bands", L"100") && !skin::Accepts(s, L"p1.l1.a1.bands", L"200"));
    CHECK(!skin::Accepts(s, L"p1.l1.a1.bands", L"40.5") && !skin::Accepts(s, L"p1.l1.a2.bands", L"40"));
    CHECK(skin::Accepts(s, L"p1.l1.a1.colors", L"#123456 0%, #ABCDEF 100%") && skin::IsAdjustmentKey(L"p1.l1.a1.smoothing"));
    CHECK(!skin::Accepts(s, L"p1.l1.a1.color", L"#123456") && !skin::Accepts(s, L"p1.l1.a1.end-color", L"#123456"));
    CHECK(s.Find(L"colors") && s.Find(L"colors")->kind == skin::SettingKind::Colors && skin::Accepts(s, L"colors", L"#FF0000 0%"));
    CHECK(!skin::Accepts(s, L"bands", L"129") && skin::Accepts(s, L"bands", L"128"));
    const std::vector<skin::Part> parts = skin::Parts(s);
    const auto part = std::find_if(parts.begin(), parts.end(), [](const skin::Part &p) { return p.key == L"p1.l1.a1"; });
    CHECK(part != parts.end() && part->label == L"频谱");
    if (part != parts.end()) {
        auto offers = [&](const wchar_t *key) {
            return std::any_of(part->adjustments.begin(), part->adjustments.end(),
                               [&](const skin::Adjustment &a) { return a.key == key; });
        };
        CHECK(offers(L"p1.l1.a1.mode") && offers(L"p1.l1.a1.peak") && offers(L"p1.l1.a1.low"));
        CHECK(!offers(L"p1.l1.a1.space"));  // nothing comes before it on its line
    }
    // A low frequency above the high one still leaves the bands an octave to spread over.
    const skin::Text &v = skin::Resolve(s, {{L"p1.l1.a1.low", L"1000"}, {L"p1.l1.a1.high", L"500"}}).panels[0].lines[0].texts[0];
    CHECK(v.audio.low == 1000.0f && v.audio.high == 2000.0f);
}

// The output device opens for listening and hands over its samples -- silence, while it plays
// nothing, which the test never makes it do. A machine without one (a CI runner) is passed over.
TEST(SpectrumListensToTheOutputDevice) {
    AudioCapture capture;
    capture.Start();
    std::vector<float> samples;
    float rate = 0.0f;
    bool open = false;
    for (int i = 0; i < 60 && !open; ++i) {
        Sleep(50);
        open = capture.Latest(&samples, &rate);
    }
    if (!open) {
        std::printf("  no output device to listen to here -- skipped\n");
        capture.Stop();
        return;
    }
    CHECK(rate >= 8000.0f && rate <= 384000.0f);
    // A tenth of a second on there is a whole window to analyse (about 85 ms), even with nothing
    // playing.
    Sleep(150);
    CHECK(capture.Latest(&samples, &rate) && samples.size() == spectrum::Analyzer::SizeFor(rate));
    spectrum::Analyzer analyzer;
    spectrum::Frame frame;
    analyzer.Run(samples.data(), samples.size(), rate, 0.0, &frame);
    CHECK(frame.power.size() == spectrum::Analyzer::SizeFor(rate) / 2 + 1);
    const ULONGLONG before = GetTickCount64();
    capture.Stop();
    CHECK(GetTickCount64() - before < 1500);
    CHECK(!capture.Latest(&samples, &rate));
}

// A spectrum's colours: one to eight, each at its place along the gradient.
TEST(SpectrumColoursAreStopsAlongAGradient) {
    std::vector<skin::Stop> stops;
    CHECK(skin::ParseStops(L"#FF0000 0%, #00FF0080 50%,#0000FF 100%", &stops) && stops.size() == 3);
    CHECK(stops[0].color == 0xFFFF0000 && stops[1].color == 0x8000FF00 && stops[1].at == 0.5f && stops[2].at == 1.0f);
    CHECK(skin::ParseStops(L"#FFFFFF 25%", &stops) && stops.size() == 1 && stops[0].at == 0.25f);
    CHECK(skin::ParseStops(L"  #102030   12.5% ,  #405060 12.5%  ", &stops) && stops.size() == 2);  // places may repeat
    CHECK(!skin::ParseStops(L"", &stops));
    CHECK(!skin::ParseStops(L"#FF0000", &stops));                      // no place
    CHECK(!skin::ParseStops(L"#FF0000 50, #00FF00 60%", &stops));      // a place without %
    CHECK(!skin::ParseStops(L"#FF0000 60%, #00FF00 50%", &stops));     // out of order
    CHECK(!skin::ParseStops(L"#FF0000 -1%", &stops) && !skin::ParseStops(L"#FF0000 101%", &stops));
    CHECK(!skin::ParseStops(L"red 0%", &stops) && !skin::ParseStops(L"#FF0000 0%,", &stops));
    std::wstring nine;
    for (int i = 0; i < 9; ++i) nine += (i ? L", " : L"") + std::wstring(L"#FFFFFF ") + std::to_wstring(i * 10) + L"%";
    CHECK(!skin::ParseStops(nine, &stops));
    skin::Skin s;
    CHECK(BuiltInComponent(L"spectrum", &s));
    const skin::Text t = skin::Resolve(s, {{L"colors", L"#FF0000 0%, #0000FF 100%"}, {L"gradient", L"level"}, {L"flow", L"6"}})
                             .panels[0].lines[0].texts[0];
    CHECK(t.audio.stops.size() == 2 && t.audio.stops[1].color == 0xFF0000FF);
    CHECK(t.audio.gradient == skin::Gradient::Level && t.audio.flow == 6.0f);
    CHECK(skin::Resolve(s, {}).panels[0].lines[0].texts[0].audio.stops.size() == 3);
}

// Three colours left to right; colour by height; colours that flow.
TEST(SpectrumDrawsItsColours) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    skin::Skin s;
    CHECK(BuiltInComponent(L"spectrum", &s));
    Picture background, out;
    background.width = 960;
    background.height = 540;
    background.bgra.assign((size_t)960 * 540 * 4, 0);
    SkinPreview preview;
    std::vector<SkinView::Bound> bounds;
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    auto draw = [&](const skin::Values &values) {
        skin::Values v = values;
        v[L"p1.s1.opacity"] = L"0";
        return preview.Render(background, skin::Resolve(s, v), ClockStyle{}, us, SYSTEMTIME{}, &out, &bounds);
    };
    auto room = [&] {
        for (const SkinView::Bound &b : bounds)
            if (b.key == L"p1.l1.a1") return b.rect;
        return D2D1_RECT_F{};
    };
    // The most of one channel (0 blue, 1 green, 2 red) in a column of the room, near its foot.
    auto most = [&](int x, int channel) {
        const D2D1_RECT_F r = room();
        int best = 0;
        for (int dx = -6; dx <= 6; ++dx)
            best = std::max<int>(best, out.bgra[((size_t)((int)r.bottom - 3) * out.width + x + dx) * 4 + channel]);
        return best;
    };
    CHECK(draw({{L"colors", L"#FF0000 0%, #00FF00 50%, #0000FF 100%"}}));
    const D2D1_RECT_F r = room();
    const int left = (int)r.left + 8, mid = (int)((r.left + r.right) / 2), right = (int)r.right - 8;
    CHECK(most(left, 2) > 200 && most(left, 1) < 80 && most(left, 0) < 40);
    CHECK(most(mid, 1) > 200 && most(mid, 2) < 80 && most(mid, 0) < 80);
    CHECK(most(right, 0) > 200 && most(right, 2) < 40 && most(right, 1) < 80);
    // By height: every bar is one colour, the shorter ones nearer the first. The sample's bars all
    // rise above half the room, so their foot is no longer pure red but toward blue.
    CHECK(draw({{L"colors", L"#FF0000 0%, #0000FF 100%"}, {L"gradient", L"level"}}));
    CHECK(most(left, 0) > 60 && most(right, 0) > 60);
    // One colour is a plain colour, whatever the direction.
    CHECK(draw({{L"colors", L"#00FF00 0%"}, {L"gradient", L"vertical"}}));
    CHECK(most(left, 1) > 200 && most(right, 1) > 200 && most(mid, 2) < 40);
    if (SUCCEEDED(co)) CoUninitialize();
}

// The settings app's moving previews: one view drawn frame after frame keeps its bands, so they rise
// with sound and fall back after it, and each frame is only the part the panel can touch.
TEST(SpectrumPreviewMovesFromFrameToFrame) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    skin::Skin s;
    CHECK(BuiltInComponent(L"spectrum", &s));
    const skin::Resolved r = skin::Resolve(s, {});
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    LivePreview live;
    Picture out;
    RECT where{};
    auto lit = [&] {
        size_t n = 0;
        for (size_t i = 3; i < out.bgra.size(); i += 4) n += out.bgra[i] > 40;
        return n;
    };
    // Noise, loud in every band: the same every run.
    std::vector<float> noise(4096);
    uint32_t seed = 12345;
    for (float &v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = ((float)(seed >> 8) / (float)(1u << 24) - 0.5f) * 0.6f;
    }
    auto frame = [&](const std::wstring &signature, const spectrum::Frame &f) {
        return live.Draw(signature, r, ClockStyle{}, us, data::Snapshot{}, 960, 540, f, &out, &where);
    };
    CHECK(frame(L"a", {{}, 0.0f, false, 0.0}));
    // Where the panel is (the bottom middle, 6% of the height tall) and nothing more.
    CHECK(where.left > 200 && where.right < 760 && where.top > 440 && where.bottom <= 540);
    CHECK((LONG)out.width == where.right - where.left && (LONG)out.height == where.bottom - where.top);
    const size_t rest = lit();
    CHECK(rest > 0);
    for (double t = 1.0 / 60.0; t < 0.3; t += 1.0 / 60.0) CHECK(frame(L"a", FrameOf(noise, t)));
    const size_t loud = lit();
    CHECK(loud > rest * 3);
    // Silence: a moment later the bands are on their way down, not gone.
    CHECK(frame(L"a", {{}, 0.0f, false, 0.32}));
    const size_t falling = lit();
    CHECK(falling > rest && falling <= loud);
    // Another component or other values start afresh, at rest.
    CHECK(frame(L"b", {{}, 0.0f, false, 0.34}));
    CHECK(lit() == rest);
    if (SUCCEEDED(co)) CoUninitialize();
}

TEST(SpectrumIsDrawnWhereItsRoomIs) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    skin::Skin s;
    CHECK(BuiltInComponent(L"spectrum", &s));
    Picture background, out;
    background.width = 960;
    background.height = 540;
    background.bgra.assign((size_t)960 * 540 * 4, 0);
    SkinPreview preview;
    std::vector<SkinView::Bound> bounds;
    const RegionalFormat us{L"en-US", L"h:mm tt", L"dddd, MMMM d, yyyy"};
    CHECK(preview.Render(background, skin::Resolve(s, {{L"p1.s1.opacity", L"0"}}), ClockStyle{}, us, SYSTEMTIME{}, &out,
                         &bounds));
    const auto room = std::find_if(bounds.begin(), bounds.end(), [](const SkinView::Bound &b) { return b.key == L"p1.l1.a1"; });
    CHECK(room != bounds.end());
    if (room != bounds.end()) {
        // 6% of the height tall, 13 times as wide, centred, 5% of the height above the bottom.
        const D2D1_RECT_F &r = room->rect;
        CHECK(std::fabs((r.bottom - r.top) - 0.06f * 540) < 1.5f);
        CHECK(std::fabs((r.right - r.left) - 13 * 0.06f * 540) < 2.0f);
        CHECK(std::fabs((r.left + r.right) / 2 - 480) < 1.5f && std::fabs(r.bottom - 0.95f * 540) < 1.5f);
        // Bars in it, blue at the left end and pink at the right; nothing above it.
        auto pixel = [&](int x, int y) { return out.bgra.data() + ((size_t)y * out.width + x) * 4; };
        auto brightest = [&](int x0, int x1) {
            const uint8_t *best = pixel(x0, (int)r.bottom - 2);
            for (int x = x0; x < x1; ++x) {
                const uint8_t *p = pixel(x, (int)r.bottom - 2);
                if (p[0] + p[1] + p[2] > best[0] + best[1] + best[2]) best = p;
            }
            return best;
        };
        const uint8_t *left = brightest((int)r.left, (int)r.left + 20), *right = brightest((int)r.right - 20, (int)r.right);
        CHECK(left[0] > 200 && left[2] < left[0]);    // 8F D3 FF: blue over red
        CHECK(right[2] > 200 && right[1] < right[2]);  // F3 A6 FF: red over green
        bool clear = true;
        for (int x = (int)r.left; x < (int)r.right; ++x) clear = clear && pixel(x, (int)r.top - 4)[1] == 0;
        CHECK(clear);
    }
    // Silent and set to hide while silent: nothing at all.
    spectrum::Frame silence;
    CHECK(preview.Render(background, skin::Resolve(s, {{L"hide", L"on"}}), ClockStyle{}, us, SYSTEMTIME{}, &out, nullptr,
                         false, nullptr, &silence));
    bool dark = true;
    for (size_t i = 0; i < out.bgra.size(); i += 4) dark = dark && out.bgra[i] == 0 && out.bgra[i + 1] == 0 && out.bgra[i + 2] == 0;
    CHECK(dark);
    // Each way of drawing it draws something.
    for (const wchar_t *mode : {L"bars", L"line", L"area"})
        for (const wchar_t *direction : {L"up", L"down", L"both"}) {
            CHECK(preview.Render(background, skin::Resolve(s, {{L"mode", mode}, {L"direction", direction},
                                                              {L"p1.l1.a1.peak", L"1"}, {L"p1.l1.a1.bass", L"middle"}}),
                                 ClockStyle{}, us, SYSTEMTIME{}, &out));
            size_t lit = 0;
            for (size_t i = 0; i < out.bgra.size(); i += 4) lit += out.bgra[i] + out.bgra[i + 1] + out.bgra[i + 2] > 60;
            CHECK(lit > 300);
        }
    if (SUCCEEDED(co)) CoUninitialize();
}
