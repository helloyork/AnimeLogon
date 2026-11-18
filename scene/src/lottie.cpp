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
}
struct Lottie::State {
    std::unique_ptr<tvg::SwCanvas> canvas;
    std::unique_ptr<tvg::LottieAnimation> animation;
    std::vector<uint32_t> pixels;
    std::map<std::wstring,Slot> slots;
    std::string applied;
    uint32_t slot = 0;
    int width = 0, height = 0;
    float drawnFrame = -1;
    float sourceWidth = 0, sourceHeight = 0;
};
Lottie::Lottie() = default;
Lottie::~Lottie() = default;
bool Lottie::Open(const std::vector<uint8_t> &bytes, std::wstring *why) {
    static Engine engine;
    if (!engine.ready) return Fail(why,L"Lottie engine initialization failed");
    rapidjson::Document json;
    json.Parse<rapidjson::kParseValidateEncodingFlag>((const char *)bytes.data(),bytes.size());
    if (json.HasParseError() || !json.IsObject()) return Fail(why,L"invalid Lottie JSON");
    for (const char *field : {"w","h","fr","ip","op"})
        if (!json.HasMember(field) || !json[field].IsNumber()) return Fail(why,L"Lottie needs dimensions and a frame range");
    if (json["w"].GetDouble() <= 0 || json["h"].GetDouble() <= 0 || json["w"].GetDouble() > 16384 || json["h"].GetDouble() > 16384 ||
        json["fr"].GetDouble() <= 0 || json["fr"].GetDouble() > 240 || json["op"].GetDouble() <= json["ip"].GetDouble())
        return Fail(why,L"invalid Lottie dimensions or frame range");
    auto s = std::make_unique<State>(); size_t nodes = 0;
    if (!Check(json,"",0,&nodes,&s->slots,why)) return false;
    s->canvas.reset(tvg::SwCanvas::gen(tvg::EngineOption::None));
    s->animation.reset(tvg::LottieAnimation::gen());
    if (!s->canvas || !s->animation || s->animation->picture()->load((const char *)bytes.data(),(uint32_t)bytes.size(),"lottie+json",nullptr,true) != tvg::Result::Success)
        return Fail(why,L"Lottie could not be loaded");
    s->animation->picture()->size(&s->sourceWidth,&s->sourceHeight);
    if (!std::isfinite(s->animation->duration()) || s->animation->duration() <= 0 || s->animation->totalFrame() <= 0 ||
        s->sourceWidth <= 0 || s->sourceHeight <= 0 ||
        s->canvas->add(s->animation->picture()) != tvg::Result::Success) return Fail(why,L"Lottie could not be prepared");
    s_ = std::move(s); return true;
}
bool Lottie::HasSlot(const std::wstring &id) const {
    const auto it = s_->slots.find(id);
    return it != s_->slots.end() && it->second != Slot::Unsupported;
}
bool Lottie::Draw(const std::map<std::wstring, std::array<float, 4>> &values, double time, int width, int height,
                  bool *changed, std::wstring *why) {
    auto &s = *s_;
    *changed = false;
    if (width != s.width || height != s.height) {
        s.pixels.assign((size_t)width*height,0);
        if (s.canvas->target(s.pixels.data(),width,width,height,tvg::ColorSpace::ARGB8888) != tvg::Result::Success) return Fail(why,L"Lottie target failed");
        // The animation fills the box it is drawn in, as a picture given a width and a height does.
        const tvg::Matrix transform{width/s.sourceWidth,0,0,0,height/s.sourceHeight,0,0,0,1};
        if (s.animation->picture()->transform(transform) != tvg::Result::Success) return Fail(why,L"Lottie transform failed");
        s.width = width; s.height = height; s.drawnFrame = -1;
    }
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    for (const auto &[id, v] : values) {
        const auto it = s.slots.find(id);
        if (it == s.slots.end() || it->second == Slot::Unsupported) return Fail(why,L"unknown Lottie slot");
        const std::string key = ToUtf8(id);
        writer.Key(key.c_str()); writer.StartObject(); writer.Key("p"); writer.StartObject(); writer.Key("k");
        const size_t n = it->second == Slot::Scalar ? 1 : it->second == Slot::Vector2 ? 2 : it->second == Slot::Vector3 ? 3 : 4;
        if (n == 1) writer.Double(std::isfinite(v[0]) ? v[0] : 0);
        else { writer.StartArray(); for (size_t i = 0; i < n; ++i) writer.Double(std::isfinite(v[i]) ? v[i] : 0); writer.EndArray(); }
        writer.EndObject(); writer.EndObject();
    }
    writer.EndObject(); const std::string slots(buffer.GetString(),buffer.GetSize());
    if (!values.empty() && slots != s.applied) {
        const uint32_t next = s.animation->gen(slots.c_str());
        if (!next) return Fail(why,L"Lottie slot override failed");
        if (s.slot) s.animation->del(s.slot);
        if (s.animation->apply(next) != tvg::Result::Success) return Fail(why,L"Lottie slot override failed");
        s.slot = next; s.applied = slots; s.drawnFrame = -1;
    }
    const double duration = s.animation->duration();
    const double fraction = std::fmod(std::max(0.0,std::isfinite(time) ? time : 0.0),duration)/duration;
    const float frame = std::floor((float)(fraction*s.animation->totalFrame()));
    if (frame == s.drawnFrame) return true;
    s.animation->frame(frame);
    if (s.canvas->update() != tvg::Result::Success || s.canvas->draw(true) != tvg::Result::Success || s.canvas->sync() != tvg::Result::Success)
        return Fail(why,L"Lottie frame rendering failed");
    s.drawnFrame = frame; *changed = true;
    return true;
}
const uint8_t *Lottie::Pixels() const { return reinterpret_cast<const uint8_t *>(s_->pixels.data()); }
float Lottie::Width() const { return s_->sourceWidth; }
float Lottie::Height() const { return s_->sourceHeight; }
float Lottie::Duration() const { return s_->animation->duration(); }
}
