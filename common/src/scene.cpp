#include "animelogon/scene.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include "animelogon/text.h"

namespace animelogon::scene {
namespace {
const wchar_t *anchors[] = {L"top-left", L"top", L"top-right", L"left", L"center", L"right",
                           L"bottom-left", L"bottom", L"bottom-right"};
bool Number(const std::wstring &s, double *n) {
    if (s.empty() || s.size() > 32) return false;
    wchar_t *end = nullptr;
    *n = wcstod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(*n) && std::abs(*n) <= 1e6;
}
bool Label(const std::wstring &s) {
    if (s.empty() || s.size() > 128) return false;
    for (wchar_t c : s) if (c < 32 || c == 127) return false;
    return true;
}
bool Ends(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
enum class Type { Float, Bool, Vector };
bool Literal(const std::wstring &s, Type type, double lo = -1e6, double hi = 1e6) {
    if (type == Type::Bool) return s == L"true" || s == L"false";
    double n;
    if (Number(s, &n)) return n >= lo && n <= hi;
    if (type == Type::Float) return false;
    if (s.find(L',') != std::wstring::npos) return !Components(s).empty();
    if (s == L"true" || s == L"false") return true;
    if ((s.size() == 7 || s.size() == 9) && s[0] == L'#') {
        for (size_t i = 1; i < s.size(); ++i) if (!iswxdigit(s[i])) return false;
        return true;
    }
    return false;
}
class Checker {
public:
    Document d;
    std::wstring error;
    bool Fail(const xml::Element &e, const std::wstring &s) {
        error = Format(L"line %d: %s", e.line, s.c_str()); return false;
    }
    bool Binding(const xml::Element &e, const std::wstring &s, Type type, double lo = -1e6, double hi = 1e6) {
        if (s.empty() || s[0] != L'$') return Literal(s, type, lo, hi) || Fail(e, L"invalid bound value " + s);
        const auto *setting = d.controls.Find(s.substr(1));
        if (!setting) return Fail(e, L"unknown setting " + s);
        if (setting->kind == skin::SettingKind::Number) {
            if (type != Type::Bool && setting->min >= lo && setting->max <= hi) return true;
        } else if (setting->kind == skin::SettingKind::Color && type == Type::Vector) return true;
        else if (setting->kind == skin::SettingKind::Choice || setting->kind == skin::SettingKind::Toggle) {
            for (const auto &o : setting->options) if (!Literal(o.value, type, lo, hi)) return Fail(e, L"setting has incompatible values");
            return true;
        }
        return Fail(e, L"setting has an incompatible type or range");
    }
    bool ReadLayer(const xml::Element &e, Layer *out, bool base) {
        Layer l;
        size_t sources = 0;
        for (const auto &[k, v] : e.attributes) {
            if (k == L"image" || k == L"video" || k == L"shader" || k == L"lottie") {
                if (++sources > 1) return Fail(e, L"a layer needs exactly one source");
                if (k == L"image") { l.kind = Kind::Image; l.image = v; }
                else {
                    l.file = v;
                    l.kind = k == L"shader" ? Kind::Shader : k == L"video" ? Kind::Video : Kind::Lottie;
                    if (!IsAssetName(v) || (l.kind == Kind::Shader && !Ends(v, L".hlsl")) ||
                        (l.kind == Kind::Lottie && !Ends(v, L".json")) || (l.kind == Kind::Video && !Ends(v, L".mp4")))
                        return Fail(e, L"invalid source file");
                }
            } else if (k == L"blend" && !base) {
                if (v == L"normal") l.blend = Blend::Normal;
                else if (v == L"add") l.blend = Blend::Add;
                else if (v == L"screen") l.blend = Blend::Screen;
                else if (v == L"multiply") l.blend = Blend::Multiply;
                else return Fail(e, L"unknown blend");
            } else if (k == L"fit" && base) {
                if (v == L"fill") l.fit = Scaling::Fill;
                else if (v == L"fit") l.fit = Scaling::Fit;
                else if (v == L"stretch") l.fit = Scaling::Stretch;
                else return Fail(e, L"unknown fit");
            } else if (k == L"reads" && v == L"below") l.readsBelow = true;
            else if (k == L"opacity" && !base) { if (!Binding(e, v, Type::Float, 0, 1)) return false; l.opacity = v; }
            else if (k == L"when" && !base) { if (!Binding(e, v, Type::Bool)) return false; l.when = v; }
            else if (k == L"anchor" && !base) {
                bool found = false;
                for (int i = 0; i < 9; ++i) if (v == anchors[i]) { l.anchor = i; found = true; }
                if (!found) return Fail(e, L"unknown anchor");
                l.full = false;
            } else if ((k == L"offset-x" || k == L"offset-y" || k == L"height") && !base) {
                double n;
                if (!Number(v, &n) || (k == L"height" ? n <= 0 || n > 100 : std::abs(n) > 100)) return Fail(e, L"invalid placement");
                if (k == L"offset-x") l.x = (float)n;
                else if (k == L"offset-y") l.y = (float)n;
                else l.height = (float)n;
                l.full = false;
            } else return Fail(e, L"unknown layer attribute " + k);
        }
        if (sources != 1 || (base && l.kind == Kind::Lottie) || (!base && (l.kind == Kind::Image || l.kind == Kind::Video)))
            return Fail(e, L"invalid layer source");
        if (l.readsBelow && (base || l.kind != Kind::Shader)) return Fail(e, L"only an effect shader can read below");
        if (l.kind == Kind::Image) {
            bool found = false;
            for (const auto &i : d.images) if (i.id == l.image) found = true;
            if (!found) return Fail(e, L"unknown image");
        }
        std::set<std::wstring> ids;
        for (const auto &b : e.children) {
            const bool param = b.name == L"param" && l.kind == Kind::Shader;
            const bool slot = b.name == L"slot" && l.kind == Kind::Lottie;
            if ((!param && !slot) || b.attributes.size() != 2 || !b.children.empty()) return Fail(b, L"invalid binding");
            const auto *id = b.Find(param ? L"slot" : L"id"), *v = b.Find(L"value");
            long long index = 0;
            if (!id || !v || (param ? !ParseInt(*id, &index) || index < 0 || index >= 16 || *id != std::to_wstring(index)
                                    : !skin::IsSettingId(*id)) || !ids.insert(*id).second) return Fail(b, L"invalid or repeated binding id");
            if (!Binding(b, *v, Type::Vector)) return false;
            (param ? l.params : l.slots).push_back({*id, *v});
        }
        *out = std::move(l); return true;
    }
    bool Run() {
        const auto &root = d.xml;
        if (root.name != L"scene" || !root.Find(L"format") || *root.Find(L"format") != L"1") return Fail(root, L"expected scene format 1");
        for (const auto &[k, v] : root.attributes) {
            if (k == L"format") continue;
            if ((k != L"name" && k != L"author") || (!Label(v) && !(k == L"author" && v.empty()))) return Fail(root, L"invalid scene attribute");
            (k == L"name" ? d.name : d.author) = v;
        }
        if (d.name.empty()) return Fail(root, L"scene needs a name");
        xml::Element controls{L"component", {{L"format", L"1"}, {L"name", d.name}}, {}, 1};
        xml::Element settings{L"settings", {}, {}, 1};
        for (const auto &e : root.children) if (e.name == L"setting") {
            const auto *kind = e.Find(L"kind");
            if (!kind || (*kind != L"number" && *kind != L"color" && *kind != L"bool" && *kind != L"choice"))
                return Fail(e, L"unknown setting kind");
            xml::Element copy = e; copy.name = *kind;
            copy.attributes.erase(std::remove_if(copy.attributes.begin(), copy.attributes.end(), [](const auto &p) { return p.first == L"kind"; }), copy.attributes.end());
            if (*kind == L"bool") {
                if (!e.children.empty()) return Fail(e, L"a bool takes no children");
                copy.name = L"choice";
                copy.children = {{L"option", {{L"value", L"true"}, {L"label", L"开"}}, {}, e.line},
                                 {L"option", {{L"value", L"false"}, {L"label", L"关"}}, {}, e.line}};
            }
            settings.children.push_back(std::move(copy));
        }
        controls.children.push_back(std::move(settings));
        controls.children.push_back({L"panel", {}, {{L"line", {}, {{L"text", {{L"value", L"x"}}, {}, 1}}, 1}}, 1});
        if (!skin::Parse(xml::Write(controls), &d.controls, &error)) return false;
        std::set<std::wstring> imageIds;
        for (const auto &e : root.children) if (e.name == L"image") {
            const auto *id = e.Find(L"id"), *src = e.Find(L"src");
            if (!id || !src || e.attributes.size() != 2 || !e.children.empty() || !skin::IsSettingId(*id) ||
                !imageIds.insert(*id).second || !IsAssetName(*src) || d.images.size() == kMaxImages ||
                !(Ends(*src, L".bmp") || Ends(*src, L".png") || Ends(*src, L".jpg") || Ends(*src, L".webp"))) return Fail(e, L"invalid image");
            d.images.push_back({*id, *src});
        }
        size_t bases = 0;
        for (const auto &e : root.children) {
            if (e.name == L"setting" || e.name == L"image") continue;
            if (e.name == L"base") { if (++bases != 1) return Fail(e, L"repeated base"); if (!ReadLayer(e, &d.base, true)) return false; }
            else if (e.name == L"layer") {
                Layer l;
                if (d.layers.size() == kMaxLayers) return Fail(e, L"too many layers");
                if (!ReadLayer(e, &l, false)) return false;
                d.layers.push_back(std::move(l));
            } else return Fail(e, L"unknown scene element");
        }
        if (bases != 1) return Fail(root, L"scene needs exactly one base");
        return true;
    }
};
void Put(std::vector<uint8_t> &out, uint32_t n) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(n >> (8 * i)));
}
uint32_t U32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
}
bool IsAssetName(std::wstring_view name) {
    if (name.empty() || name.size() > 64) return false;
    const size_t slash = name.find(L'/');
    if (slash != std::wstring_view::npos) {
        const auto dir = name.substr(0, slash);
        if (dir != L"images" && dir != L"shaders" && dir != L"lottie") return false;
        name.remove_prefix(slash + 1);
    }
    const size_t dot = name.find(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot > 32 || dot == name.size() - 1) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (i == dot) continue;
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || (i < dot && (c == L'_' || c == L'-')))) return false;
    }
    const auto stem = name.substr(0, dot);
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") && stem[3] >= L'1' && stem[3] <= L'9')) return false;
    return true;
}
bool Parse(std::string_view text, Document *out, std::wstring *why) {
    Checker c;
    if (!xml::Parse(text, &c.d.xml, &c.error) || !c.Run()) { if (why) *why = c.error; return false; }
    *out = std::move(c.d); return true;
}
std::string Normalize(const Document &d) { return xml::Write(d.xml); }
std::wstring BytecodeName(const std::wstring &s) { return s.substr(0, s.size() - 5) + L".dxbc"; }
std::vector<std::wstring> Assets(const Document &d, bool compiled) {
    std::set<std::wstring> names;
    for (const auto &i : d.images) names.insert(i.file);
    auto layer = [&](const Layer &l) {
        if (l.kind == Kind::Image) return;
        names.insert(l.file);
        if (compiled && l.kind == Kind::Shader) names.insert(BytecodeName(l.file));
    };
    layer(d.base); for (const auto &l : d.layers) layer(l);
    if (compiled && d.base.kind == Kind::Video) names.insert(L"images/video.bmp");
    return {names.begin(), names.end()};
}
const File *Find(const Files &files, const std::wstring &name) {
    for (const auto &f : files) if (f.name == name) return &f;
    return nullptr;
}
bool Accepts(const Document &d, const std::wstring &id, const std::wstring &v) {
    if (const auto *s = d.controls.Find(id)) return skin::IsValue(*s, v);
    for (const auto &p : Parts(d)) for (const auto &a : p.adjustments) if (a.key == id) {
        if (a.control == skin::Control::Choice) {
            for (const auto &o : a.options) if (o.value == v) return true;
            return false;
        }
        double n = 0; return Number(v,&n) && n >= a.min && n <= a.max;
    }
    return false;
}
std::vector<skin::Part> Parts(const Document &d) {
    std::vector<skin::Part> result;
    const wchar_t *labels[] = {L"左上",L"上方",L"右上",L"左侧",L"居中",L"右侧",L"左下",L"下方",L"右下"};
    for (size_t i = 0; i < d.layers.size(); ++i) {
        skin::Part p; p.key = L"p"+std::to_wstring(i+1);
        p.label = (d.layers[i].kind == Kind::Lottie ? L"动画层 " : L"效果层 ")+std::to_wstring(i+1);
        auto add = [&](const wchar_t *key, const wchar_t *label, double lo, double hi, double step) {
            skin::Adjustment a; a.key = p.key+L"."+key; a.label = label; a.min = lo; a.max = hi; a.step = step;
            p.adjustments.push_back(std::move(a));
        };
        skin::Adjustment anchor; anchor.key = p.key+L".anchor"; anchor.label = L"位置"; anchor.control = skin::Control::Choice;
        anchor.options.push_back({L"full",L"铺满画面"});
        for (int j = 0; j < 9; ++j) anchor.options.push_back({anchors[j],labels[j]});
        p.adjustments.push_back(std::move(anchor));
        add(L"offset-x",L"水平偏移",-100,100,.1); add(L"offset-y",L"垂直偏移",-100,100,.1);
        add(L"size",L"高度",.1,100,.1); add(L"opacity",L"透明度",0,1,.01);
        skin::Adjustment visible; visible.key = p.key+L".visible"; visible.label = L"显示";
        visible.control = skin::Control::Choice; visible.options = {{L"true",L"开"},{L"false",L"关"}};
        p.adjustments.push_back(std::move(visible)); result.push_back(std::move(p));
    }
    return result;
}
std::wstring Effective(const Document &d, const skin::Values &values, const std::wstring &key) {
    if (const auto *s = d.controls.Find(key)) return skin::ValueOf(*s,values);
    if (const auto it = values.find(key); it != values.end() && Accepts(d,key,it->second)) return it->second;
    for (size_t i = 0; i < d.layers.size(); ++i) {
        const auto &l = d.layers[i]; const std::wstring prefix = L"p"+std::to_wstring(i+1)+L".";
        if (key == prefix+L"anchor") return l.full ? L"full" : anchors[l.anchor];
        if (key == prefix+L"offset-x") return Format(L"%.4g",l.x);
        if (key == prefix+L"offset-y") return Format(L"%.4g",l.y);
        if (key == prefix+L"size") return Format(L"%.4g",l.height);
        if (key == prefix+L"opacity") return BindingText(d,values,l.opacity);
        if (key == prefix+L"visible") return BindingText(d,values,l.when);
    }
    return {};
}
Layer Placed(const Document &d, const skin::Values &values, size_t index) {
    Layer l = d.layers.at(index); const std::wstring p = L"p"+std::to_wstring(index+1)+L".";
    const auto anchor = Effective(d,values,p+L"anchor"); l.full = anchor == L"full";
    for (int j = 0; j < 9; ++j) if (anchor == anchors[j]) l.anchor = j;
    l.x = (float)wcstod(Effective(d,values,p+L"offset-x").c_str(),nullptr);
    l.y = (float)wcstod(Effective(d,values,p+L"offset-y").c_str(),nullptr);
    l.height = (float)wcstod(Effective(d,values,p+L"size").c_str(),nullptr);
    l.opacity = Effective(d,values,p+L"opacity"); l.when = Effective(d,values,p+L"visible"); return l;
}
Document Bake(const Document &d, const skin::Values &values, std::wstring *error) {
    auto root = d.xml; size_t layer = 0;
    auto set = [](xml::Element &e, const std::wstring &key, const std::wstring &value) {
        for (auto &a : e.attributes) if (a.first == key) { a.second = value; return; }
        e.attributes.emplace_back(key,value);
    };
    for (auto &e : root.children) {
        if (e.name == L"setting") {
            const auto *id = e.Find(L"id"); const auto it = id ? values.find(*id) : values.end();
            if (it != values.end() && Accepts(d,it->first,it->second)) set(e,L"default",it->second);
        } else if (e.name == L"layer") {
            const std::wstring p = L"p"+std::to_wstring(++layer)+L".";
            for (const auto *key : {L"anchor",L"offset-x",L"offset-y",L"size",L"opacity",L"visible"}) {
                const auto it = values.find(p+key); if (it == values.end() || !Accepts(d,it->first,it->second)) continue;
                if (std::wstring(key) == L"anchor" && it->second == L"full") {
                    e.attributes.erase(std::remove_if(e.attributes.begin(),e.attributes.end(),[](const auto &a) {
                        return a.first == L"anchor" || a.first == L"height" || a.first == L"offset-x" || a.first == L"offset-y";
                    }),e.attributes.end());
                } else if (Effective(d,values,p+L"anchor") != L"full" || std::wstring(key) == L"opacity" || std::wstring(key) == L"visible")
                    set(e,std::wstring(key) == L"size" ? L"height" : std::wstring(key) == L"visible" ? L"when" : key,it->second);
            }
        }
    }
    Document result; std::wstring why;
    if (Parse(xml::Write(root),&result,&why)) { if (error) error->clear(); return result; }
    if (error) *error=why;
    return d;
}
std::wstring BindingText(const Document &d, const skin::Values &values, const std::wstring &binding) {
    std::wstring s = binding;
    if (!s.empty() && s[0] == L'$') {
        const auto *control = d.controls.Find(s.substr(1));
        if (!control) return {};
        s = skin::ValueOf(*control, values);
    }
    return s;
}
std::vector<float> Components(const std::wstring &s) {
    if (s == L"true") return {1};
    if (s == L"false") return {0};
    if (!s.empty() && s[0] == L'#') {
        if (s.size() != 7 && s.size() != 9) return {};
        for (size_t i = 1; i < s.size(); ++i) if (!iswxdigit(s[i])) return {};
        const uint32_t rgb = wcstoul(s.substr(1, 6).c_str(), nullptr, 16);
        const float a = s.size() == 9 ? wcstoul(s.substr(7, 2).c_str(), nullptr, 16) / 255.f : 1.f;
        return {(rgb >> 16) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    }
    std::vector<float> result; size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(L',',start);
        double n = 0;
        if (result.size() == 4 || !Number(s.substr(start,end == std::wstring::npos ? s.size()-start : end-start),&n)) return {};
        result.push_back((float)n);
        if (end == std::wstring::npos) break;
        start = end+1;
    }
    return result;
}
std::array<float, 4> Value(const Document &d, const skin::Values &values, const std::wstring &binding) {
    std::array<float,4> result{};
    const auto parts = Components(BindingText(d,values,binding));
    std::copy(parts.begin(),parts.end(),result.begin()); return result;
}
std::vector<uint8_t> Pack(const Files &files) {
    if (files.size() > 40) return {};
    std::vector<uint8_t> out;
    Put(out, 0x314E4353); Put(out, (uint32_t)files.size());
    std::set<std::wstring> names;
    for (const auto &f : files) {
        if ((f.name != L"scene.xml" && f.name != L"still.bmp" && !IsAssetName(f.name)) || !names.insert(f.name).second ||
            f.bytes.size() > kMaxBundleBytes || out.size() + 8 + f.name.size() + f.bytes.size() > kMaxBundleBytes) return {};
        const std::string name = ToUtf8(f.name);
        Put(out, (uint32_t)name.size()); Put(out, (uint32_t)f.bytes.size());
        out.insert(out.end(), name.begin(), name.end()); out.insert(out.end(), f.bytes.begin(), f.bytes.end());
    }
    return out;
}
bool Unpack(const uint8_t *bytes, size_t size, Files *files, std::wstring *why) {
    auto fail = [&] { if (why) *why = L"invalid scene envelope"; return false; };
    if (!bytes || size < 8 || size > kMaxBundleBytes || U32(bytes) != 0x314E4353 || U32(bytes + 4) > 40) return fail();
    Files out; std::set<std::wstring> names; size_t at = 8;
    for (uint32_t i = 0; i < U32(bytes + 4); ++i) {
        if (size - at < 8) return fail();
        const size_t n = U32(bytes + at), m = U32(bytes + at + 4); at += 8;
        if (!n || n > 64 || n > size - at || m > size - at - n) return fail();
        const std::string_view name((const char *)bytes + at, n); const std::wstring w = FromUtf8(name); at += n;
        if (ToUtf8(w) != name || (w != L"scene.xml" && w != L"still.bmp" && !IsAssetName(w)) || !names.insert(w).second) return fail();
        out.push_back({w, {bytes + at, bytes + at + m}}); at += m;
    }
    if (at != size) return fail();
    *files = std::move(out); return true;
}
}  // namespace animelogon::scene
