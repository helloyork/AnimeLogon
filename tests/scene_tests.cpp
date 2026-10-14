#include "check.h"
#include "animelogon/scene.h"
#include "animelogon/package.h"
using namespace animelogon;
namespace {
std::string Scene(const std::string &body) { return "<scene format=\"1\" name=\"test\">"+body+"</scene>"; }
bool Parses(const std::string &body) { scene::Document d; std::wstring why; return scene::Parse(Scene(body),&d,&why); }
}
TEST(SceneRejectsExecutableMarkupAndPaths) {
    CHECK(!Parses("<base shader=\"../x.hlsl\"/>"));
    CHECK(!Parses("<base shader=\"C:/x.hlsl\"/>"));
    CHECK(!Parses("<base shader=\"shaders/x.hlsl\"/><script/>"));
    CHECK(!Parses("<base shader=\"con.hlsl\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\" onload=\"x\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\">hello</base>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"/><base shader=\"y.hlsl\"/>"));
    CHECK(!Parses("<layer shader=\"x.hlsl\"/>"));
    CHECK(!Parses("<base lottie=\"x.json\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\" reads=\"below\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"/><layer image=\"x\"/>"));
}
TEST(SceneChecksBindingTypesAndLimits) {
    const std::string number = "<setting id=\"n\" kind=\"number\" label=\"n\" min=\"0\" max=\"2\" default=\"1\"/>";
    CHECK(Parses(number+"<base shader=\"x.hlsl\"><param slot=\"15\" value=\"$n\"/></base>"));
    CHECK(!Parses(number+"<base shader=\"x.hlsl\"/><layer shader=\"y.hlsl\" opacity=\"$n\"/>"));
    CHECK(!Parses(number+"<base shader=\"x.hlsl\"/><layer shader=\"y.hlsl\" when=\"$n\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"><param slot=\"0\" value=\"$missing\"/></base>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"><param slot=\"16\" value=\"1\"/></base>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"><param slot=\"0\" value=\"1\"/><param slot=\"0\" value=\"2\"/></base>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"/><layer shader=\"y.hlsl\" opacity=\"nan\"/>"));
    CHECK(!Parses("<base shader=\"x.hlsl\"/><layer shader=\"y.hlsl\" height=\"0\"/>"));
    std::string layers = "<base shader=\"x.hlsl\"/>";
    for (int i=0;i<8;++i) layers += "<layer shader=\"x.hlsl\"/>";
    CHECK(Parses(layers)); CHECK(!Parses(layers+"<layer shader=\"x.hlsl\"/>"));
}
TEST(SceneResolvesTypedOverridesAndNormalizes) {
    scene::Document d; std::wstring why;
    CHECK(scene::Parse(Scene("<setting id=\"tint\" kind=\"color\" label=\"Tint\" default=\"#ff0000\"/>"
        "<setting id=\"show\" kind=\"bool\" label=\"Show\" default=\"true\"/>"
        "<image id=\"city\" src=\"city.png\"/><base image=\"city\"/><layer shader=\"rain.hlsl\" when=\"$show\">"
        "<param slot=\"0\" value=\"$tint\"/></layer>"),&d,&why));
    CHECK(scene::Value(d,{},L"$tint")[0] == 1);
    CHECK(scene::Value(d,{{L"tint",L"#00ff00"}},L"$tint")[1] == 1);
    CHECK(scene::Value(d,{{L"tint",L"junk"}},L"$tint")[0] == 1);
    CHECK(scene::Value(d,{{L"show",L"false"}},L"$show")[0] == 0);
    CHECK(!scene::Accepts(d,L"show",L"1"));
    scene::Document again; CHECK(scene::Parse(scene::Normalize(d),&again,&why));
    CHECK(scene::Assets(again,true) == std::vector<std::wstring>({L"city.png",L"rain.dxbc",L"rain.hlsl"}));
}
TEST(SceneEnvelopeRejectsOverflowDuplicatesAndTrailingData) {
    scene::Files files{{L"scene.xml",{1,2,3}},{L"shaders/a.hlsl",{4,5}}}, again;
    const auto bytes = scene::Pack(files); std::wstring why;
    CHECK(scene::Unpack(bytes.data(),bytes.size(),&again,&why) && again.size() == 2 && again[1].bytes == files[1].bytes);
    for (size_t n=0;n<bytes.size();++n) CHECK(!scene::Unpack(bytes.data(),n,&again,&why));
    auto bad = bytes; bad.push_back(0); CHECK(!scene::Unpack(bad.data(),bad.size(),&again,&why));
    bad = bytes; bad[8] = 255; bad[9] = 255; CHECK(!scene::Unpack(bad.data(),bad.size(),&again,&why));
    files.push_back(files[0]); CHECK(scene::Pack(files).empty());
    CHECK(scene::Pack({{L"../evil.hlsl",{1}}}).empty());
}
TEST(SceneCoordinateBindingsStayFiniteAndTyped) {
    CHECK(scene::Components(L"10,-20,30") == std::vector<float>({10,-20,30}));
    CHECK(scene::Components(L"1,2,3,4,5").empty());
    CHECK(scene::Components(L"1,nan").empty());
    CHECK(scene::Components(L"1,inf").empty());
    CHECK(scene::Components(L"1,").empty());
    CHECK(scene::Components(L"#zz0000").empty());
    CHECK(Parses("<base shader=\"sky.hlsl\"/><layer lottie=\"bird.json\"><slot id=\"position\" value=\"20,40\"/></layer>"));
    CHECK(!Parses("<base shader=\"sky.hlsl\"/><layer lottie=\"bird.json\"><slot id=\"position\" value=\"20,nan\"/></layer>"));
}
TEST(SceneArchiveCannotAddAssetsToAThemeArchive) {
    std::vector<uint8_t> zip; std::wstring why;
    CHECK(package::WriteToMemory({{"scene.xml",L"",{'x'}},{"sky.hlsl",L"",{'x'}}},&zip,&why));
    package::Reader r; CHECK(r.OpenBytes(zip,&why) && r.Find("scene.xml"));
    CHECK(!package::WriteToMemory({{"theme.xml",L"",{'x'}},{"sky.hlsl",L"",{'x'}}},&zip,&why));
    CHECK(!package::WriteToMemory({{"theme.xml",L"",{'x'}},{"scene.xml",L"",{'x'}}},&zip,&why));
}
