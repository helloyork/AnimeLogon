#include "check.h"
#include <cmath>
#include <cstdio>
#include "animelogon/scenegraph.h"
using namespace animelogon;
namespace sg = animelogon::scenegraph;
namespace {
std::string Scene(const std::string &body, const std::string &head = "") {
    return "<scene format=\"2\" name=\"test\" width=\"1920\" height=\"1080\"" + head + ">" + body + "</scene>";
}
const std::string kAssets =
    "<assets><image id=\"sky\" file=\"images/sky.png\"/><image id=\"hero\" file=\"images/Hero.PNG\"/>"
    "<mask id=\"hair\" file=\"masks/hair.png\"/><frames id=\"blink\"><frame file=\"e0.png\"/><frame file=\"e1.png\"/>"
    "<frame file=\"e2.png\"/></frames><shader id=\"fx\" kind=\"effect\" file=\"fx.hlsl\"/>"
    "<shader id=\"rays\" kind=\"layer\" file=\"rays.hlsl\"/><lottie id=\"birds\" file=\"birds.json\"/></assets>";
bool Parses(const std::string &body, std::wstring *why = nullptr) {
    sg::Document d; std::wstring w;
    const bool ok = sg::Parse(Scene(body), sg::Mode::Package, &d, &w);
    if (why) *why = w;
    return ok;
}
bool Near(float a, double b) { return std::abs(a - b) < 1e-3; }
const sg::Placed *Find(const std::vector<sg::Placed> &nodes, const wchar_t *id) {
    for (const auto &p : nodes) {
        if (p.node->id == id) return &p;
        if (const auto *c = Find(p.children, id)) return c;
    }
    return nullptr;
}
}  // namespace

TEST(SceneGraphReadsTheProposalSample) {
    const std::string text = R"(<scene format="2" name="樱花庭院" author="Nomen" width="1920" height="1080" still="2.5">
  <settings>
    <group label="天气">
      <toggle id="petals" label="飘落樱花" default="on"/>
      <number id="amount" label="樱花量" min="5" max="120" step="1" default="40" when="$petals"/>
    </group>
    <group label="人物">
      <image id="hero" label="人物图片" default="hero"/>
      <number id="sway" label="头发摆动" min="0" max="1" step="0.05" default="0.5"/>
    </group>
    <color id="lantern" label="灯笼颜色" default="#FFB86B"/>
  </settings>
  <assets>
    <image id="sky" file="images/sky.png"/>
    <image id="garden" file="images/garden.png"/>
    <image id="hero" file="images/hero.png"/>
    <image id="petal" file="images/petal.png"/>
    <mask id="hair" file="masks/hair.png"/>
    <mask id="chest" file="masks/chest.png"/>
    <mask id="lantern" file="masks/lantern.png"/>
    <frames id="blink"><frame file="images/eyes-0.png"/><frame file="images/eyes-1.png"/><frame file="images/eyes-2.png"/></frames>
    <image id="night" file="images/night-tint.png"/>
  </assets>
  <camera>
    <wander property="x" amplitude="12" period="40"/>
    <drive property="scale" by="wake" from="1" to="1.04"/>
  </camera>
  <layer id="sky" source="sky" full="true" depth="0.05">
    <effect id="clouds" type="pan" speed-x="0.004"/>
  </layer>
  <layer id="garden" source="garden" full="true" depth="0.3"/>
  <group id="girl" depth="0.6" x="1180" y="1080" anchor="bottom" height="980">
    <layer id="body" source="$hero">
      <effect type="bend" mask="hair" strength="$sway" period="3.2" root="top"/>
      <effect type="breathe" mask="chest" amount="0.008" period="4.5" pivot="0.5 0.9"/>
    </layer>
    <layer id="eyes" source="blink" x="236" y="214" fps="24" repeat="bounce" gap="2.5-6"/>
  </group>
  <layer id="lantern-glow" source="garden" full="true" depth="0.3" blend="add" opacity="0.6">
    <effect id="glow" type="glow" mask="lantern" color="$lantern" strength="0.8"/>
    <drive property="glow.strength" by="audio.bass" from="0.6" to="1.2"/>
  </layer>
  <layer id="night" source="night" full="true" blend="multiply" when="clock.period=night"/>
  <particles id="petals" preset="petals" when="$petals" rate="$amount" source="petal" emit="top" wind="0.2" seed="7" depth="0.8"/>
</scene>)";
    sg::Document d; std::wstring why;
    const bool parsed = sg::Parse(text, sg::Mode::Package, &d, &why);
    CHECK(parsed);
    if (!parsed) { std::fwprintf(stderr, L"    %s\n", why.c_str()); return; }
    CHECK(d.name == L"樱花庭院" && d.width == 1920 && d.still == 2.5);
    CHECK(d.settings.size() == 5);
    if (d.settings.size() != 5) return;
    CHECK(d.settings[1].group == L"天气" && d.settings[1].when == L"$petals" && d.settings[2].image);
    CHECK(d.assets.size() == 9 && d.nodes.size() == 6 && d.camera.size() == 2 && sg::Moves(d));

    // In the library every file has a name derived from its asset id, and nothing else is accepted.
    sg::Document stored;
    const bool storedOk = sg::Stored(d, &stored, &why);
    CHECK(storedOk);
    if (!storedOk) { std::fwprintf(stderr, L"    %s\n", why.c_str()); return; }
    CHECK(stored.mode == sg::Mode::Stored && stored.assets[0].files[0] == L"textures/sky.tex");
    CHECK(stored.FindAsset(L"blink")->files[2] == L"textures/blink-2.tex");
    sg::Document again;
    CHECK(sg::Parse(sg::Write(stored), sg::Mode::Stored, &again, &why));
    CHECK(!sg::Parse(sg::Write(d), sg::Mode::Stored, &again, &why));
    CHECK(sg::StoredNames(d).size() == 11);

    // Settings: the petal amount shows only while petals are on; the hero picture can be any library image.
    sg::Values values;
    CHECK(sg::Shown(d, values, *d.FindSetting(L"amount")));
    values[L"petals"] = L"off";
    CHECK(!sg::Shown(d, values, *d.FindSetting(L"amount")));
    CHECK(sg::Accepts(d, L"hero", L"0123456789abcdef") && !sg::Accepts(d, L"hero", L"../x") && !sg::Accepts(d, L"amount", L"500"));

    sg::Inputs in; in.time = 1; in.day = 0.5;
    values = {{L"hero", L"0123456789abcdef"}};
    const sg::Frame f = sg::Evaluate(d, values, in);
    CHECK(Find(f.nodes, L"night") == nullptr && Find(f.nodes, L"petals") != nullptr);
    CHECK(Find(f.nodes, L"body")->source == L"wallpaper:0123456789abcdef");
    CHECK(Near(Find(f.nodes, L"lantern-glow")->effects[0].values.at(L"strength")[0], 0.6));
    in.day = 0.9; in.audio[1] = 1;
    const sg::Frame g = sg::Evaluate(d, {}, in);
    CHECK(Find(g.nodes, L"night") != nullptr && Find(g.nodes, L"body")->source == L"hero");
    CHECK(Near(Find(g.nodes, L"lantern-glow")->effects[0].values.at(L"strength")[0], 1.2));
}

TEST(SceneGraphRejectsWhatItDoesNotKnow) {
    std::wstring why;
    CHECK(Parses(kAssets + "<layer source=\"sky\"/>", &why));
    CHECK(!Parses(kAssets + "<layer source=\"sky\"/><script/>"));
    CHECK(!Parses(kAssets + "<layer source=\"sky\" onload=\"x\"/>"));
    CHECK(!Parses(kAssets + "<layer source=\"missing\"/>"));
    CHECK(!Parses(kAssets + "<layer source=\"hair\"/>"));                 // a mask is not a picture to draw
    CHECK(!Parses(kAssets + "<layer source=\"fx\"/>"));                   // an effect shader is not a layer
    CHECK(!Parses(kAssets));                                              // nothing to draw
    CHECK(!Parses(kAssets + "<layer source=\"sky\" opacity=\"2\"/>"));
    CHECK(!Parses(kAssets + "<layer source=\"sky\" opacity=\"nan\"/>"));
    CHECK(!Parses(kAssets + "<layer source=\"sky\" anchor=\"middle\"/>"));
    CHECK(!Parses(kAssets + "<layer id=\"a\" source=\"sky\"/><layer id=\"a\" source=\"sky\"/>"));
    for (const char *file : {"../x.png", "C:/x.png", "/x.png", "a\\\\x.png", ".x.png", "con.png", "x.gif", "a/b/c/d/e.png"})
        CHECK(!Parses(std::string("<assets><image id=\"x\" file=\"") + file + "\"/></assets><layer source=\"x\"/>"));
    CHECK(!Parses("<assets><video id=\"a\" file=\"a.mp4\"/><video id=\"b\" file=\"b.mp4\"/></assets><layer source=\"a\"/>"));
    sg::Document d;
    CHECK(!sg::Parse("<scene format=\"1\" name=\"x\"/>", sg::Mode::Package, &d, &why) && why.find(L"format 2") != std::wstring::npos);
    CHECK(!sg::Parse("<scene format=\"3\" name=\"x\"/>", sg::Mode::Package, &d, &why) && why.find(L"newer") != std::wstring::npos);
}

TEST(SceneGraphKeepsItsLimits) {
    std::string many = kAssets;
    for (int i = 0; i < 64; ++i) many += "<layer source=\"sky\"/>";
    CHECK(Parses(many));
    CHECK(!Parses(many + "<layer source=\"sky\"/>"));
    std::string nest = "<layer source=\"sky\"/>";
    for (int i = 0; i < 4; ++i) nest = "<group>" + nest + "</group>";
    CHECK(Parses(kAssets + nest));
    CHECK(!Parses(kAssets + "<group>" + nest + "</group>"));
    std::string effects;
    for (int i = 0; i < 8; ++i) effects += "<effect type=\"blur\"/>";
    CHECK(Parses(kAssets + "<layer source=\"sky\">" + effects + "</layer>"));
    CHECK(!Parses(kAssets + "<layer source=\"sky\">" + effects + "<effect type=\"blur\"/></layer>"));
    CHECK(Parses(kAssets + "<particles max=\"20000\"/>"));
    CHECK(!Parses(kAssets + "<particles max=\"15000\"/><particles max=\"6000\"/>"));
}

TEST(SceneGraphChecksBindingsAndMotions) {
    const std::string settings =
        "<settings><number id=\"n\" label=\"n\" min=\"0\" max=\"1\" default=\"0.5\"/><number id=\"big\" label=\"b\" min=\"0\" max=\"9\" default=\"1\"/>"
        "<toggle id=\"t\" label=\"t\" default=\"on\" on=\"1\" off=\"0\"/><choice id=\"outfit\" label=\"o\" default=\"sky\">"
        "<option value=\"sky\" label=\"a\"/><option value=\"hero\" label=\"b\"/></choice>"
        "<choice id=\"bad\" label=\"x\" default=\"sky\"><option value=\"sky\" label=\"a\"/><option value=\"nope\" label=\"b\"/></choice>"
        "<image id=\"pic\" label=\"p\" default=\"sky\"/></settings>";
    const std::string head = settings + kAssets;
    CHECK(Parses(head + "<layer source=\"sky\" opacity=\"$n\"/>"));
    CHECK(Parses(head + "<layer source=\"sky\" opacity=\"$t\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" opacity=\"$big\"/>"));         // 0-9 does not fit 0-1
    CHECK(Parses(head + "<layer source=\"$outfit\"/>"));                       // a choice between pictures
    CHECK(!Parses(head + "<layer source=\"$bad\"/>"));
    CHECK(Parses(head + "<layer source=\"$pic\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" opacity=\"$pic\"/>"));
    CHECK(Parses(head + "<layer source=\"sky\" when=\"$t\"/><layer source=\"sky\" when=\"$outfit=hero\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" when=\"$outfit=other\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" when=\"$n\"/>"));
    CHECK(Parses(head + "<layer source=\"sky\" when=\"media.state=playing\"/><layer source=\"sky\" when=\"!battery.level\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" when=\"media.state=loud\"/>"));
    CHECK(!Parses(head + "<layer source=\"sky\" when=\"weather\"/>"));
    // Motions aim at animated properties of the same node, and only one keys or drive sets a property.
    CHECK(Parses(head + "<layer source=\"sky\"><keys property=\"x\" duration=\"2\"><key at=\"0\" value=\"0\"/><key at=\"2\" value=\"10\"/></keys>"
                        "<wave property=\"x\" amplitude=\"3\" period=\"2\"/></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><keys property=\"anchor\" duration=\"2\"><key at=\"0\" value=\"0\"/></keys></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><keys property=\"x\" duration=\"2\"><key at=\"1\" value=\"0\"/><key at=\"0.5\" value=\"1\"/></keys></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><keys property=\"x\" duration=\"2\"><key at=\"3\" value=\"0\"/></keys></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><drive property=\"x\" by=\"wake\" from=\"0\" to=\"1\"/><drive property=\"x\" by=\"wake\" from=\"0\" to=\"1\"/></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><drive property=\"x\" by=\"media.state\" from=\"0\" to=\"1\"/></layer>"));
    CHECK(Parses(head + "<layer source=\"sky\"><effect id=\"g\" type=\"glow\"/><drive property=\"g.strength\" by=\"$big\" from=\"0\" to=\"4\"/></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><effect id=\"g\" type=\"glow\"/><drive property=\"g.root\" by=\"wake\" from=\"0\" to=\"1\"/></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><effect type=\"bend\" spin=\"1\"/></layer>"));
    CHECK(!Parses(head + "<layer source=\"sky\"><effect type=\"bend\" mask=\"sky\"/></layer>"));
    CHECK(Parses(head + "<layer source=\"sky\"><effect id=\"c\" source=\"fx\"><param slot=\"3\" value=\"$n\"/></effect>"
                        "<wave property=\"c.3\" amplitude=\"1\" period=\"1\"/></layer>"));
    CHECK(!Parses("<settings><toggle id=\"a\" label=\"a\" default=\"on\" when=\"$a\"/></settings>" + kAssets + "<layer source=\"sky\"/>"));
    CHECK(!Parses("<settings><font id=\"f\" label=\"f\" default=\"\"/></settings>" + kAssets + "<layer source=\"sky\"/>"));
}

TEST(SceneGraphMotionsAreFunctionsOfTime) {
    const std::string text = Scene(kAssets +
        "<settings><toggle id=\"show\" label=\"s\" default=\"on\"/></settings>"
        "<camera><wander property=\"x\" amplitude=\"10\" period=\"5\"/></camera>"
        "<layer id=\"k\" source=\"sky\"><keys property=\"x\" duration=\"4\" repeat=\"bounce\" delay=\"1\">"
        "<key at=\"0\" value=\"0\"/><key at=\"4\" value=\"100\"/></keys><wave property=\"y\" amplitude=\"2\" period=\"4\"/></layer>"
        "<layer id=\"once\" source=\"sky\"><keys property=\"opacity\" duration=\"2\" repeat=\"once\"><key at=\"0\" value=\"0\" ease=\"smooth\"/>"
        "<key at=\"2\" value=\"1\"/></keys></layer>"
        "<fill id=\"c\" color=\"#000000\"><drive property=\"color\" by=\"wake\" from=\"#000000\" to=\"#FF8000\"/></fill>"
        "<layer id=\"hidden\" source=\"sky\" when=\"$show\"/>"
        "<layer id=\"eyes\" source=\"blink\" fps=\"10\" repeat=\"bounce\" gap=\"2-4\"/>");
    sg::Document d; std::wstring why;
    const bool parsed = sg::Parse(text, sg::Mode::Package, &d, &why);
    CHECK(parsed);
    if (!parsed) { std::fwprintf(stderr, L"    %s\n", why.c_str()); return; }
    sg::Inputs in;
    in.time = 3;  // two seconds past the delay: halfway up; the wave three quarters through its period
    sg::Frame f = sg::Evaluate(d, {}, in);
    CHECK(Near(Find(f.nodes, L"k")->x, 50));
    CHECK(Near(Find(f.nodes, L"k")->y, -2));
    in.time = 7;  // six seconds past the delay: on the way back, halfway down
    f = sg::Evaluate(d, {}, in);
    CHECK(Near(Find(f.nodes, L"k")->x, 50));
    in.time = 2;  // the wave half through its period: back at zero; the one-off fade at its end
    f = sg::Evaluate(d, {}, in);
    CHECK(Near(Find(f.nodes, L"k")->y, 0) && Near(Find(f.nodes, L"once")->opacity, 1));
    in.time = 1;  // a quarter: the wave at its top; the smooth fade halfway
    f = sg::Evaluate(d, {}, in);
    CHECK(Near(Find(f.nodes, L"k")->y, 2) && Near(Find(f.nodes, L"once")->opacity, 0.5));
    in.wake = 0.5;
    f = sg::Evaluate(d, {}, in);
    const auto c = Find(f.nodes, L"c")->values.at(L"color");
    CHECK(Near(c[0], 0.5) && Near(c[1], 0.25098) && Near(c[2], 0));
    CHECK(Find(sg::Evaluate(d, {{L"show", L"off"}}, in).nodes, L"hidden") == nullptr);

    // The same moment always gives the same frame; the camera wanders within its amplitude.
    for (double t : {0.0, 12.5, 3600.25}) {
        in.time = t;
        const sg::Frame a = sg::Evaluate(d, {}, in), b = sg::Evaluate(d, {}, in);
        CHECK(a.cameraX == b.cameraX && std::abs(a.cameraX) <= 10.0f && Find(a.nodes, L"eyes")->frame == Find(b.nodes, L"eyes")->frame);
    }
}

TEST(SceneGraphGapsRestBetweenRunsForTheirRange) {
    // A 0.5 s run with 2-4 s rests: sample finely and measure the rests between runs.
    double last = -1, restStart = -1;
    int runs = 0;
    bool inRun = false, good = true;
    for (int i = 0; i <= 200000; ++i) {
        const double t = i * 0.001;
        const bool playing = sg::Local(t, 0.5, sg::Repeat::Once, 0, 2, 4, 1234) > 0;
        if (playing && !inRun) {
            if (restStart >= 0) { const double rest = t - restStart; good = good && rest >= 2 - 0.01 && rest <= 4 + 0.01; }
            ++runs; inRun = true;
        } else if (!playing && inRun) { restStart = t; inRun = false; }
        last = t;
    }
    CHECK(good && runs > 50 && runs < 100 && last > 199);
    // Bounce goes there and back; without a gap a loop wraps around.
    CHECK(std::abs(sg::Local(1.5, 1, sg::Repeat::Bounce, 0, 0, 0, 0) - 0.5) < 1e-9);
    CHECK(std::abs(sg::Local(2.25, 1, sg::Repeat::Loop, 0, 0, 0, 0) - 0.25) < 1e-9);
    CHECK(sg::Local(5, 1, sg::Repeat::Once, 0, 0, 0, 0) == 1);
}

TEST(SceneGraphBakesTheUsersValuesAsDefaults) {
    const std::string text = Scene("<settings><color id=\"tint\" label=\"t\" default=\"#FF0000\"/><image id=\"pic\" label=\"p\" default=\"sky\"/></settings>" +
                                   kAssets + "<layer source=\"$pic\"><effect id=\"t\" type=\"tint\" color=\"$tint\"/></layer>");
    sg::Document d, baked; std::wstring why;
    const bool parsed = sg::Parse(text, sg::Mode::Package, &d, &why);
    CHECK(parsed);
    if (!parsed) { std::fwprintf(stderr, L"    %s\n", why.c_str()); return; }
    const sg::Values values{{L"tint", L"#00FF00"}, {L"pic", L"0123456789abcdef"}};
    const bool bakedOk = sg::Bake(d, values, &baked, &why);
    CHECK(bakedOk);
    if (!bakedOk) return;
    CHECK(sg::ValueOf(baked, {}, L"tint") == L"#00FF00" && sg::ValueOf(baked, {}, L"pic") == L"sky");
    sg::Inputs in;
    const auto a = sg::Evaluate(d, {{L"tint", L"#00FF00"}}, in).nodes[0].effects[0].values.at(L"color");
    const auto b = sg::Evaluate(baked, {}, in).nodes[0].effects[0].values.at(L"color");
    CHECK(a == b && Near(b[1], 1));
    // Writing and reading back changes nothing.
    sg::Document again;
    CHECK(sg::Parse(sg::Write(baked), sg::Mode::Package, &again, &why) && sg::Write(again) == sg::Write(baked));
}

TEST(SceneGraphKnowsWhenNothingMoves) {
    sg::Document d; std::wstring why;
    CHECK(sg::Parse(Scene(kAssets + "<layer source=\"sky\"><effect type=\"tint\"/></layer>"), sg::Mode::Package, &d, &why));
    CHECK(!sg::Moves(d));
    CHECK(sg::Parse(Scene(kAssets + "<layer source=\"sky\"><effect type=\"pan\" speed-x=\"0.1\"/></layer>"), sg::Mode::Package, &d, &why));
    CHECK(sg::Moves(d));
    CHECK(sg::Parse(Scene(kAssets + "<layer source=\"birds\"/>"), sg::Mode::Package, &d, &why));
    CHECK(sg::Moves(d));
    CHECK(sg::EffectTypes().size() == 17);
    // The sound is listened to only for a scene that follows it, or has a shader that could.
    CHECK(sg::ReadsSound(d));  // kAssets has a shader
    const std::string sky = "<assets><image id=\"sky\" file=\"sky.png\"/></assets>";
    CHECK(sg::Parse(Scene(sky + "<layer source=\"sky\"><drive property=\"opacity\" by=\"audio.bass\" from=\"0.5\" to=\"1\"/></layer>"),
                    sg::Mode::Package, &d, &why));
    CHECK(sg::ReadsSound(d));
    CHECK(sg::Parse(Scene(sky + "<layer source=\"sky\" when=\"!audio.level\"/>"), sg::Mode::Package, &d, &why));
    CHECK(sg::ReadsSound(d));
    CHECK(sg::Parse(Scene(sky + "<camera><drive property=\"scale\" by=\"wake\" from=\"1\" to=\"1.1\"/></camera><layer source=\"sky\"/>"),
                    sg::Mode::Package, &d, &why));
    CHECK(!sg::ReadsSound(d));
}

TEST(SceneGraphEnvelopesAreBoundedAndExact) {
    const sg::Files files{{L"scene.xml", {'<', '/'}}, {L"textures/sky.tex", {1, 2, 3}}, {L"poster.bmp", {}}};
    const auto packed = sg::Pack(files);
    sg::Files back;
    std::wstring why;
    CHECK(!packed.empty() && sg::Unpack(packed.data(), packed.size(), &back, &why) && back.size() == 3 &&
          back[1].name == L"textures/sky.tex" && back[1].bytes == files[1].bytes);
    // Trailing bytes, a cut-off envelope, repeated names and names that climb out are refused.
    auto longer = packed; longer.push_back(0);
    CHECK(!sg::Unpack(longer.data(), longer.size(), &back, &why));
    CHECK(!sg::Unpack(packed.data(), packed.size() - 1, &back, &why));
    CHECK(sg::Pack({{L"a.png", {}}, {L"a.png", {}}}).empty());
    CHECK(sg::Pack({{L"../a.png", {}}}).empty() && sg::Pack({{L"c:/a.png", {}}}).empty() && sg::Pack({{L"aux.png", {}}}).empty());
}

TEST(SceneGraphKnowsTheFirstFormatAndTheLibraryFiles) {
    CHECK(sg::FormatOf("<scene format=\"1\" name=\"old\"><base shader=\"s.hlsl\"/></scene>") == 1);
    CHECK(sg::FormatOf(Scene("<fill/>")) == 2 && sg::FormatOf("<theme/>") == 0 && sg::FormatOf("not xml") == 0);
    // A scene exported from the library carries its textures as they are.
    sg::Document d;
    std::wstring why;
    CHECK(Parses("<assets><image id=\"sky\" file=\"textures/sky.tex\"/><mask id=\"m\" file=\"textures/m.tex\"/></assets><fill/>", &why));
    if (!why.empty()) std::wprintf(L"    %s\n", why.c_str());
    CHECK(sg::Parse(Scene("<assets><image id=\"sky\" file=\"textures/sky.tex\"/><video id=\"clip\" file=\"video.mp4\"/>"
                          "<lottie id=\"birds\" file=\"lottie/birds.json\"/><shader id=\"rays\" kind=\"layer\" file=\"shaders/rays.hlsl\"/></assets><fill/>"),
                    sg::Mode::Stored, &d, &why));
    if (!why.empty()) std::wprintf(L"    %s\n", why.c_str());
    const std::vector<std::wstring> files{L"textures/sky.tex", L"textures/clip.tex", L"video.mp4", L"lottie/birds.json",
                                          L"shaders/rays.hlsl", L"shaders/rays.dxbc"};
    CHECK(sg::LibraryFiles(d) == files);
}
