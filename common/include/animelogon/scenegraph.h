// Scene format 2: a bounded, declarative 2D node tree. This file reads, checks and writes the
// document and works out what every node looks like at a moment. Nothing here decodes pictures,
// compiles shaders or draws: content files are only ever opened inside scene.exe.
//
// A frame depends on nothing but the moment, the inputs (clock, battery, media, sound, waking)
// and the user's values, so any moment can be computed directly: the still picture, a preview
// scrubbed to a time, thumbnails and tests all use the same Evaluate().
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "animelogon/skin.h"
#include "animelogon/xml.h"

namespace animelogon::scenegraph {

constexpr int kFormat = 2;
constexpr size_t kMaxNodes = 64, kMaxNesting = 4, kMaxEffectsPerNode = 8, kMaxEffects = 32;
constexpr size_t kMaxEmitters = 8, kMaxParticles = 20000, kMaxImages = 32, kMaxFrames = 120;
constexpr size_t kMaxVideos = 1, kMaxLotties = 4, kMaxShaders = 8, kMaxSettings = 32;
constexpr size_t kMaxMotionsPerNode = 16, kMaxKeys = 64, kMaxTextBytes = 256 * 1024;

// How asset files are named: as an author writes them in a package, or by the canonical names a
// scene has once it is in the library (derived from the asset ids).
enum class Mode { Package, Stored };

enum class AssetKind { Image, Mask, Frames, Video, Lottie, LayerShader, EffectShader };
struct Asset {
    std::wstring id;
    AssetKind kind = AssetKind::Image;
    std::vector<std::wstring> files;  // one file, or one per frame in order
    bool exact = false;               // kept uncompressed instead of BC7
    int line = 0;
};

enum class NodeKind { Layer, Fill, Particles, Group };
enum class MotionKind { Keys, Wave, Wander, Drive };
enum class Repeat { Once, Loop, Bounce };
enum class Ease { Linear, Smooth, In, Out, Step, Bezier };

using Attributes = std::vector<std::pair<std::wstring, std::wstring>>;  // in written order

struct Key {
    double at = 0;
    std::wstring value;  // a literal, or $setting
    Ease ease = Ease::Linear;
    std::array<float, 4> bezier{};
};

// <keys> and <drive> set a property's value (at most one of them per property); <wave> and
// <wander> add to it.
struct Motion {
    MotionKind kind = MotionKind::Keys;
    std::wstring property;  // "x", "opacity", "<effect id>.<parameter>"
    double duration = 0, delay = 0, gapMin = 0, gapMax = 0;
    Repeat repeat = Repeat::Loop;
    std::vector<Key> keys;
    std::wstring amplitude, period, phase;  // wave, wander: literals or $setting
    std::wstring by, from, to;              // drive: a data name or $setting, and the values it maps onto
    double inLow = 0, inHigh = 1;           // drive: the input range mapped onto from..to
    int line = 0;
};

struct Effect {
    std::wstring id, type, source, mask, when;  // a built-in `type`, or the `source` of an effect shader
    Attributes params;                          // built-in parameters by name; a shader's slots "0".."15"
    int line = 0;
};

struct Node {
    NodeKind kind = NodeKind::Layer;
    std::wstring id;
    Attributes attributes;  // every attribute but id, already checked against the node's kind
    std::vector<Effect> effects;
    std::vector<Motion> motions;
    std::vector<Node> children;  // groups only
    Attributes params;           // a shader layer's <param slot value>, a Lottie layer's <slot id value>
    int line = 0;
    const std::wstring *Find(std::wstring_view name) const;
};

struct Setting {
    skin::Setting base;    // for <image>: id, label, detail and the default asset id in `fallback`
    bool image = false;
    std::wstring group;    // the label of the <group> it is in, empty when in none
    std::wstring when;     // shown in the settings only while this holds ("$id", "$id=value", "!$id")
};

struct Document {
    Mode mode = Mode::Package;
    std::wstring name, author;
    int width = 1920, height = 1080;
    double still = 0;
    std::vector<Setting> settings;
    std::vector<Asset> assets;
    std::vector<Motion> camera;
    std::vector<Node> nodes;
    xml::Element xml;
    const Asset *FindAsset(std::wstring_view id) const;
    const Setting *FindSetting(std::wstring_view id) const;
};

bool Parse(std::string_view utf8, Mode mode, Document *out, std::wstring *why);
// The canonical text of the document as parsed.
std::string Write(const Document &document);
// Each asset file as written in the package, and the name it has in the library, in a stable order.
std::vector<std::pair<std::wstring, std::wstring>> StoredNames(const Document &document);
// The document as it is kept in the library: file names replaced by their stored names.
bool Stored(const Document &package, Document *out, std::wstring *why);

// The user's values; missing or unacceptable ones fall back to the defaults.
using Values = std::map<std::wstring, std::wstring>;
bool Accepts(const Document &document, const std::wstring &id, const std::wstring &value);
std::wstring ValueOf(const Document &document, const Values &values, const std::wstring &id);
// Whether the settings window shows this setting, given the other values.
bool Shown(const Document &document, const Values &values, const Setting &setting);
// The document with each setting's default replaced by the user's value, as a theme exports it.
// An <image> setting keeps its default: the picture the user chose is not part of the scene.
bool Bake(const Document &document, const Values &values, Document *out, std::wstring *why);

// Everything a scene can read at a moment. Fractions are -1 when unknown.
struct Inputs {
    double time = 0;     // seconds since the scene started
    double day = 0;      // 0..1 through the local day
    double wake = 0;     // 0 at rest, 1 when the wake fade has finished
    double battery = -1, progress = -1;
    int month = 1;       // 1..12
    std::array<double, 4> audio{};  // level, bass, mid, treble, 0..1
    std::wstring batteryState = L"none", powerSource = L"none", mediaState = L"none";
};
// clock.period and date.season, worked out from the inputs.
std::wstring Period(const Inputs &inputs);
std::wstring Season(const Inputs &inputs);

enum class Space { Stage, Screen };
enum class Blend { Normal, Add, Screen, Multiply };
enum class Fit { Cover, Contain, Stretch };

struct PlacedEffect {
    const Effect *effect = nullptr;
    std::wstring mask;                                   // resolved asset id, empty for none
    std::map<std::wstring, std::array<float, 4>> values; // every parameter, defaults filled in
};

// A node as it is at one moment. Positions are in stage points (space=stage) or percent of the
// screen height (space=screen); mapping onto a display is the renderer's job.
struct Placed {
    const Node *node = nullptr;
    std::wstring source, mask;  // resolved asset ids
    float x = 0, y = 0, width = 0, height = 0;  // width/height 0: the source's own size
    float pivotX = .5f, pivotY = .5f, rotation = 0, scaleX = 1, scaleY = 1, opacity = 1, depth = 1;
    int anchor = 0;  // 0..8, top-left to bottom-right
    Space space = Space::Stage;
    Blend blend = Blend::Normal;
    Fit fit = Fit::Cover;
    bool full = false;
    int frame = 0;                // which frame of a frame sequence
    double localTime = 0;         // where a Lottie or shader layer is in its own time
    std::map<std::wstring, std::wstring> text;            // every attribute as written, settings substituted
    std::map<std::wstring, std::array<float, 4>> values;  // numeric and color attributes after motions
    std::vector<PlacedEffect> effects;
    std::vector<Placed> children;
};

struct Frame {
    float cameraX = 0, cameraY = 0, cameraScale = 1, cameraRotation = 0;
    std::vector<Placed> nodes;  // only the visible ones, in drawing order
};
Frame Evaluate(const Document &document, const Values &values, const Inputs &inputs);

// Where a run of `length` seconds is at time t, for keys and frame sequences: starts after `delay`,
// repeats as `repeat` says, and with gapMax > 0 rests at its start between runs for a pseudo-random
// time from gapMin to gapMax (picked from `seed`), worked out directly for any t.
double Local(double t, double length, Repeat repeat, double delay, double gapMin, double gapMax, uint32_t seed);

// Whether any part of the scene changes with time or inputs; a scene that does not can be drawn once.
bool Moves(const Document &document);
// Whether an effect changes from frame to frame by itself: a built-in one that animates (bend, grain
// and the like), or an author's shader, which can read the time and the inputs.
bool EffectMoves(const Effect &effect);
// Whether the scene can follow the sound playing: it drives something by audio.*, shows something
// on a condition about it, or has a shader, which can read it.
bool ReadsSound(const Document &document);

// Numbers, colors (#RRGGBB[AA]) and two to four space-separated numbers, as floats.
std::vector<float> Components(const std::wstring &literal);

// The names of the built-in effects, in documentation order.
std::vector<std::wstring> EffectTypes();

// What the reader takes, from the same tables it checks against, for the format's reference.
struct AttributeInfo {
    std::wstring element;  // "node" (every node), "layer", "fill", "particles", or "effect <type>"
    std::wstring name;
    std::wstring values;   // the values taken, in words
    std::wstring fallback; // the value when the attribute is not written, empty for none
    bool animated = false; // whether a motion can move it
};
std::vector<AttributeInfo> AttributeTable();
struct DataInfo {
    std::wstring name;
    std::vector<std::wstring> words;  // a state's words; empty for a number
};
std::vector<DataInfo> DataNames();
// Every particle preset's values: preset, attribute, value.
std::vector<std::array<std::wstring, 3>> PresetValues();

// --- Files ---------------------------------------------------------------------------------
// A scene's files by name, as they travel to and from scene.exe in one envelope with bounded
// counts, names and lengths and no pointers.
constexpr size_t kMaxFiles = 4096;
constexpr size_t kMaxBundleBytes = 640ull * 1024 * 1024;
constexpr size_t kMaxTextureBytes = 512ull * 1024 * 1024;  // every texture together, in video memory
constexpr size_t kMaxShaderBytes = 64 * 1024, kMaxCompiledBytes = 1024 * 1024, kMaxLottieBytes = 16 * 1024 * 1024;
struct File {
    std::wstring name;
    std::vector<uint8_t> bytes;
};
using Files = std::vector<File>;
const File *Find(const Files &files, std::wstring_view name);
// Empty when the files do not fit an envelope.
std::vector<uint8_t> Pack(const Files &files);
bool Unpack(const uint8_t *bytes, size_t size, Files *files, std::wstring *why);

// A file inside a package: up to four segments of letters, digits, '.', '_' and '-', none
// starting with a dot and none a device name.
bool IsPackagePath(std::wstring_view path);
// The `format` a scene.xml declares: 2 for this one, 1 for the first scene format, 0 when the text
// is not a scene at all.
int FormatOf(std::string_view utf8);
// Where a shader's compiled form is kept in the library, beside its source.
std::wstring CompiledName(const Asset &asset);
// Every file a stored scene keeps in the library besides scene.xml and still.bmp, in a stable
// order: textures (a video's first frame is one), Lottie files, shader sources with their compiled
// forms, and video.mp4.
std::vector<std::wstring> LibraryFiles(const Document &stored);
// A video's first frame, kept as a texture so the renderer knows its shape before frames arrive.
std::wstring PosterName(const Asset &video);

}  // namespace animelogon::scenegraph
