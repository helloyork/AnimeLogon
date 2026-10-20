#include "lottie.h"
#include <thorvg_lottie.h>
#pragma warning(push)
#pragma warning(disable: 4390)
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#pragma warning(pop)
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "animelogon/text.h"
namespace animelogon::scene {
namespace {
using J = rapidjson::Value;
bool Fail(std::wstring *why, const wchar_t *s) { if (why) *why = s; return false; }
struct Engine {
    bool ready = tvg::Initializer::init(0) == tvg::Result::Success;
    ~Engine() { if (ready) tvg::Initializer::term(); }
};
enum class Slot { Scalar, Color, Vector2, Vector3, Unsupported };
Slot Type(const J &v, std::string_view key) {
    if (!v.HasMember("k")) return Slot::Unsupported;
    const J *k = &v["k"];
    if (k->IsArray() && !k->Empty() && (*k)[0].IsObject()) {
        const auto &first = (*k)[0];
        if (!first.HasMember("s")) return Slot::Unsupported;
        k = &first["s"];
    }
    if (k->IsNumber() || (k->IsArray() && k->Size() == 1 && (*k)[0].IsNumber())) return Slot::Scalar;
    if (!k->IsArray()) return Slot::Unsupported;
    for (const auto &n : k->GetArray()) if (!n.IsNumber()) return Slot::Unsupported;
    if (key == "c" && (k->Size() == 3 || k->Size() == 4)) return Slot::Color;
    if (k->Size() == 2) return Slot::Vector2;
    if (k->Size() == 3) return Slot::Vector3;
    return Slot::Unsupported;
}
bool Check(const J &v, std::string_view key, unsigned depth, size_t *nodes, std::map<std::wstring,Slot> *slots, std::wstring *why) {
    if (depth > 64 || ++*nodes > 500000) return Fail(why,L"Lottie structure is too complex");
    if (v.IsNumber() && (!std::isfinite(v.GetDouble()) || std::abs(v.GetDouble()) > 1e9)) return Fail(why,L"invalid Lottie number");
    if (v.IsArray()) {
        for (const auto &x : v.GetArray()) if (!Check(x,key,depth+1,nodes,slots,why)) return false;
    } else if (v.IsObject()) {
        std::set<std::string> keys;
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            const auto &m = *it;
            const std::string name(m.name.GetString(),m.name.GetStringLength());
            if (!keys.insert(name).second) return Fail(why,L"repeated Lottie property");
            if (name == "x" && m.value.IsString()) return Fail(why,L"Lottie expressions are not supported");
            if (!Check(m.value,name,depth+1,nodes,slots,why)) return false;
        }
        if (key == "layers" && v.HasMember("ty") && v["ty"].IsInt() && (v["ty"].GetInt() < 0 || v["ty"].GetInt() > 4))
            return Fail(why,L"convert Lottie text and media layers to shapes");
        if (v.HasMember("p") && v["p"].IsString()) {
            const std::string_view p(v["p"].GetString(),v["p"].GetStringLength());
            if (!v.HasMember("e") || !v["e"].IsInt() || v["e"].GetInt() != 1 ||
                (p.substr(0,22) != "data:image/png;base64," && p.substr(0,23) != "data:image/jpeg;base64,") ||
                (v.HasMember("u") && (!v["u"].IsString() || v["u"].GetStringLength())))
                return Fail(why,L"embed Lottie images as PNG or JPEG; external resources are not supported");
        }
        if (v.HasMember("sid") && v["sid"].IsString()) {
            const std::wstring id = FromUtf8({v["sid"].GetString(),v["sid"].GetStringLength()});
            const Slot type = Type(v,key);
            const auto [it,added] = slots->emplace(id,type);
            if (!added && it->second != type) return Fail(why,L"a Lottie slot has conflicting types");
        }
    }
    return true;
}
bool Matches(Slot type, const std::wstring &text) {
    const auto values = Components(text);
    if (type == Slot::Color) return !text.empty() && text[0] == L'#' && values.size() == 4;
    if (type == Slot::Scalar) return values.size() == 1;
    if (type == Slot::Vector2) return values.size() == 2;
    if (type == Slot::Vector3) return values.size() == 2 || values.size() == 3;
    return false;
}
}
struct Lottie::State {
    std::unique_ptr<tvg::SwCanvas> canvas;
    std::unique_ptr<tvg::LottieAnimation> animation;
    std::vector<uint32_t> pixels;
    std::map<std::wstring,Slot> slots;
    std::string applied;
    uint32_t slot = 0;
    int width = 0, height = 0;
    float sourceWidth = 0, sourceHeight = 0;
};
Lottie::Lottie() = default;
Lottie::~Lottie() = default;
bool Lottie::Open(const File &file, const Document &d, const Layer &layer, std::wstring *why) {
    static Engine engine;
    if (!engine.ready) return Fail(why,L"Lottie engine initialization failed");
    rapidjson::Document json;
    json.Parse<rapidjson::kParseValidateEncodingFlag>((const char *)file.bytes.data(),file.bytes.size());
    if (json.HasParseError() || !json.IsObject()) return Fail(why,L"invalid Lottie JSON");
    for (const char *field : {"w","h","fr","ip","op"})
        if (!json.HasMember(field) || !json[field].IsNumber()) return Fail(why,L"Lottie needs dimensions and a frame range");
    if (json["w"].GetDouble() <= 0 || json["h"].GetDouble() <= 0 || json["w"].GetDouble() > 16384 || json["h"].GetDouble() > 16384 ||
        json["fr"].GetDouble() <= 0 || json["fr"].GetDouble() > 240 || json["op"].GetDouble() <= json["ip"].GetDouble())
        return Fail(why,L"invalid Lottie dimensions or frame range");
    auto s = std::make_unique<State>(); size_t nodes = 0;
    if (!Check(json,"",0,&nodes,&s->slots,why)) return false;
    for (const auto &b : layer.slots) {
        const auto it = s->slots.find(b.id);
        if (it == s->slots.end() || !Matches(it->second,BindingText(d,{},b.value))) return Fail(why,L"unknown or incompatible Lottie slot binding");
        if (!b.value.empty() && b.value[0] == L'$') {
            const auto *control = d.controls.Find(b.value.substr(1));
            for (const auto &option : control->options)
                if (!Matches(it->second,option.value)) return Fail(why,L"Lottie slot choice has incompatible values");
        }
    }
    s->canvas.reset(tvg::SwCanvas::gen(tvg::EngineOption::None));
    s->animation.reset(tvg::LottieAnimation::gen());
    if (!s->canvas || !s->animation || s->animation->picture()->load((const char *)file.bytes.data(),(uint32_t)file.bytes.size(),"lottie+json",nullptr,true) != tvg::Result::Success)
        return Fail(why,L"Lottie could not be loaded");
    s->animation->picture()->size(&s->sourceWidth,&s->sourceHeight);
    if (!std::isfinite(s->animation->duration()) || s->animation->duration() <= 0 || s->animation->totalFrame() <= 0 ||
        s->canvas->add(s->animation->picture()) != tvg::Result::Success) return Fail(why,L"Lottie could not be prepared");
    s_ = std::move(s); return true;
}
bool Lottie::Draw(const Document &d, const Layer &layer, const Inputs &in, int width, int height, std::wstring *why) {
    auto &s = *s_;
    if (width != s.width || height != s.height) {
        s.pixels.assign((size_t)width*height,0);
        if (s.canvas->target(s.pixels.data(),width,width,height,tvg::ColorSpace::ARGB8888) != tvg::Result::Success) return Fail(why,L"Lottie target failed");
        const float ratio = std::max(width/s.sourceWidth,height/s.sourceHeight);
        const tvg::Matrix transform{ratio,0,(width-s.sourceWidth*ratio)/2,0,ratio,(height-s.sourceHeight*ratio)/2,0,0,1};
        if (s.animation->picture()->transform(transform) != tvg::Result::Success) return Fail(why,L"Lottie transform failed");
        s.width = width; s.height = height;
    }
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    for (const auto &b : layer.slots) {
        const auto text = BindingText(d,in.values,b.value); const auto type = s.slots.at(b.id);
        if (!Matches(type,text)) return Fail(why,L"incompatible Lottie slot value");
        auto values = Components(text);
        const std::string id = ToUtf8(b.id); writer.Key(id.c_str()); writer.StartObject(); writer.Key("p"); writer.StartObject(); writer.Key("k");
        if (type == Slot::Scalar) writer.Double(values[0]);
        else {
            if (type == Slot::Vector3 && values.size() == 2) values.push_back(0);
            writer.StartArray(); for (float n : values) writer.Double(n); writer.EndArray();
        }
        writer.EndObject(); writer.EndObject();
    }
    writer.EndObject(); const std::string slots(buffer.GetString(),buffer.GetSize());
    if (!layer.slots.empty() && slots != s.applied) {
        const uint32_t next = s.animation->gen(slots.c_str());
        if (!next) return Fail(why,L"Lottie slot override failed");
        if (s.slot) s.animation->del(s.slot);
        if (s.animation->apply(next) != tvg::Result::Success) return Fail(why,L"Lottie slot override failed");
        s.slot = next; s.applied = slots;
    }
    const float fraction = std::fmod(std::max(0.f,in.time),s.animation->duration())/s.animation->duration();
    s.animation->frame(fraction*s.animation->totalFrame());
    if (s.canvas->update() != tvg::Result::Success || s.canvas->draw(true) != tvg::Result::Success || s.canvas->sync() != tvg::Result::Success)
        return Fail(why,L"Lottie frame rendering failed");
    return true;
}
const uint8_t *Lottie::Pixels() const { return reinterpret_cast<const uint8_t *>(s_->pixels.data()); }
float Lottie::Aspect() const { return s_->sourceWidth/s_->sourceHeight; }
float Lottie::Duration() const { return s_->animation->duration(); }
}
