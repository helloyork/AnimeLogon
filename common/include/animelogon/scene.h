// A bounded, declarative scene. Content parsers and shader creation belong in scene.exe.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>
#include "animelogon/skin.h"
#include "animelogon/settings.h"

namespace animelogon::scene {
constexpr size_t kMaxLayers = 8, kMaxImages = 4, kMaxShaderBytes = 64 * 1024;
constexpr size_t kMaxLottieBytes = 16 * 1024 * 1024, kMaxBundleBytes = 256 * 1024 * 1024;
enum class Kind { Image, Video, Shader, Lottie };
enum class Blend { Normal, Add, Screen, Multiply };
struct Image { std::wstring id, file; };
struct Binding { std::wstring id, value; };
struct Layer {
    Kind kind = Kind::Shader;
    Blend blend = Blend::Normal;
    std::wstring file, image, opacity = L"1", when = L"true";
    Scaling fit = Scaling::Fill;
    bool readsBelow = false, full = true;
    int anchor = 4;
    float x = 0, y = 0, height = 100;
    std::vector<Binding> params, slots;
};
struct Document {
    std::wstring name, author;
    skin::Skin controls;
    std::vector<Image> images;
    Layer base;
    std::vector<Layer> layers;
    xml::Element xml;
};
struct File { std::wstring name; std::vector<uint8_t> bytes; };
using Files = std::vector<File>;

bool IsAssetName(std::wstring_view name);
bool Parse(std::string_view utf8, Document *out, std::wstring *why);
std::string Normalize(const Document &document);
// Every content file, once, in a stable order. Compiled shaders are included on request.
std::vector<std::wstring> Assets(const Document &document, bool compiled);
std::wstring BytecodeName(const std::wstring &source);
const File *Find(const Files &files, const std::wstring &name);
bool Accepts(const Document &document, const std::wstring &id, const std::wstring &value);
std::wstring BindingText(const Document &document, const skin::Values &values, const std::wstring &binding);
// Scalars, colors and two to four comma-separated finite coordinates.
std::vector<float> Components(const std::wstring &literal);
std::array<float, 4> Value(const Document &document, const skin::Values &values, const std::wstring &binding);
// The binary envelope has bounded counts, names, and lengths; it contains no pointers.
std::vector<uint8_t> Pack(const Files &files);
bool Unpack(const uint8_t *bytes, size_t size, Files *files, std::wstring *why);
}  // namespace animelogon::scene
