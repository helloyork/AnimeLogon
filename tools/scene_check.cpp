// Checks AnimeLogon scenes with the reader AnimeLogon itself uses, says what a scene costs to show,
// and writes the attribute reference generated from the scene reader. Not installed.
//
//   scene_check <path>...
//       Checks each path: a scene package (.alscene) or a folder laid out as one (scene.xml and the
//       files it names). scene.xml is read as the importer reads it, and errors name the line.
//       Every file it names must be there; pictures are measured, shaders compiled to the shader
//       contract. Then it prints what the scene holds and how much video memory its pictures take
//       once compressed, warns about what makes a scene slow or blurry, and refuses what the
//       importer refuses (over 512 MB of textures). A Lottie file is only checked when imported.
//   scene_check --pack <folder> <out.alscene>
//       Checks the folder as above and writes it as an .alscene (a zip of stored entries).
//   scene_check --write-reference <file>
//   scene_check --check-reference <file>
//       Writes the attribute reference generated from the scene reader, or checks that <file> is
//       that reference, line endings aside.
//
// Exit codes: 0 all good (warnings allowed), 1 something was refused or differs, 2 usage.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "animelogon/package.h"
#include "animelogon/scenegraph.h"
#include "animelogon/text.h"
#include "animelogon/texture.h"
#include "animelogon/xml.h"
#include "contract.h"

using namespace animelogon;
namespace fs = std::filesystem;
namespace sg = scenegraph;

namespace {

// What each attribute does, for the reference. A new attribute without a line here fails
// --check-reference, so the reference cannot fall behind the reader.
const std::map<std::wstring, std::wstring> kAbout = {
    {L"node.x", L"Where the node's anchor point goes, across: in stage points, or in its group's units inside a group. With "
                L"`space=\"screen\"`, in percent of the screen's height from the screen's anchor corner."},
    {L"node.y", L"The same, down."},
    {L"node.width", L"The box's width. Unwritten (or 0), a layer takes its source's size, a group the extent of its children "
                    L"and a fill or particle emitter the box it is in; when only one of `width` and `height` is written, the "
                    L"other keeps that natural shape. A group's children scale with its box."},
    {L"node.height", L"The box's height, as for `width`."},
    {L"node.anchor", L"Which point of the box `x` and `y` place. With `space=\"screen\"`, also which corner or edge of the "
                     L"screen they are measured from."},
    {L"node.space", L"`stage`: placed on the stage, which covers each screen and may be cut at its sides. `screen`: placed "
                    L"against the screen itself, so it is never cut; its size still follows the stage's scale."},
    {L"node.full", L"Covers the box it is in (the stage, at the top level) as `fit` says, centred. `x`, `y`, `width`, "
                   L"`height` and `anchor` are then ignored."},
    {L"node.fit", L"How a `full` node covers its box: all of it, cutting the overflow (`cover`); all of the node inside it "
                  L"(`contain`); or stretched to it."},
    {L"node.pivot", L"The point of the box that `rotation` and `scale` turn and grow about, as fractions of its width and "
                    L"height."},
    {L"node.rotation", L"Degrees clockwise about the pivot."},
    {L"node.scale", L"Grows or shrinks the node about its pivot."},
    {L"node.scale-x", L"Grows or shrinks it across only, on top of `scale`."},
    {L"node.scale-y", L"Grows or shrinks it down only, on top of `scale`."},
    {L"node.opacity", L"Opacity of everything the node draws. A group with an opacity below 1 is drawn as one picture and "
                      L"then faded."},
    {L"node.blend", L"How it lands on what is below: over it (`normal`), adding light (`add`), lightening (`screen`) or "
                    L"darkening (`multiply`)."},
    {L"node.depth", L"How far the camera's motion moves it: 0 not at all (far away), 1 fully (near). Only nodes at the top "
                    L"level take part; a group's children move with the group. A full-screen overlay should say `0`."},
    {L"node.mask", L"A mask over the node's box: it shows where the mask is white and is hidden where it is black."},
    {L"layer.source", L"What the layer shows. A frame sequence plays; a video plays stretched to the box; a Lottie animation "
                      L"is drawn at the size it covers; a layer shader draws the box."},
    {L"layer.fps", L"Frames a second of a frame sequence."},
    {L"layer.repeat", L"How a frame sequence plays: once (stopping on its last frame), over and over, or forwards and back."},
    {L"layer.gap", L"A frame sequence rests on its first frame this long between runs, a different time from the range "
                   L"each run. `2.5-6` with `repeat=\"bounce\"` blinks an eye."},
    {L"fill.color", L"The fill's colour."},
    {L"fill.colors", L"A gradient instead of one colour."},
    {L"fill.gradient", L"Which way the gradient runs: top to bottom, or left to right."},
    {L"particles.source", L"What each particle looks like. Unwritten: a soft dot, or a streak when `stretch` is above 0."},
    {L"particles.preset", L"A set of defaults (see Presets below). Anything written on the element wins."},
    {L"particles.rate", L"Particles born a second, at most `max` alive at once. The user's particle density setting thins "
                        L"them."},
    {L"particles.lifetime", L"Seconds each particle lives, give or take a fifth."},
    {L"particles.emit", L"Where they are born: just outside one edge of the box, at its centre (`point`), along its "
                        L"middle across (`line`), or anywhere in it (`area`)."},
    {L"particles.speed", L"Speed at birth, in points a second, give or take a fifth."},
    {L"particles.angle", L"Direction at birth, in degrees: 0 right, 90 down, 180 left, 270 up."},
    {L"particles.spread", L"Directions at birth spread over this many degrees around `angle`."},
    {L"particles.gravity", L"Downward pull, in points a second each second; negative pulls up."},
    {L"particles.wind", L"The air's drift across, in points a second; particles move with it."},
    {L"particles.drag", L"How fast a particle's own speed fades, a second; with gravity it settles at gravity / drag."},
    {L"particles.spin", L"Degrees a second each particle turns, half to twice as fast and either way. Each starts at a "
                        L"random angle."},
    {L"particles.size", L"Size in points, give or take 30 percent."},
    {L"particles.size-end", L"Size at the end of a particle's life; unwritten, the same as `size`."},
    {L"particles.opacity-end", L"Opacity at the end of a particle's life; unwritten, 1."},
    {L"particles.color", L"Colour, which also tints a picture."},
    {L"particles.color-end", L"Colour at the end of a particle's life; unwritten, the same as `color`."},
    {L"particles.fade", L"Share of a particle's life it fades in over, and out over at the end."},
    {L"particles.seed", L"Picks which pseudo-random particles appear. Two emitters with the same settings and seed look the "
                        L"same."},
    {L"particles.max", L"Most particles alive at once. All emitters together hold at most 20 000."},
    {L"particles.fps", L"Frames a second when `source` is a frame sequence."},
    {L"particles.sway", L"Side-to-side drift, in points, each particle at its own pace (2 to 5 seconds a swing); half as "
                        L"much up and down."},
    {L"particles.twinkle", L"How far each particle's brightness rises and falls, at its own pace."},
    {L"particles.stretch", L"Draws each particle as a streak along its motion, as long as it moves in this many seconds."},
    // Effects. Distances are fractions of the node's box height unless said otherwise.
    {L"effect bend.strength", L"How far the far end swings: 1 is 12 percent of the box's length from its root."},
    {L"effect bend.period", L"Seconds for one swing there and back."},
    {L"effect bend.root", L"The edge that stays still."},
    {L"effect bend.waves", L"How far the far end lags behind the root, in whole swings: 0 swings it stiffly."},
    {L"effect breathe.amount", L"How much it grows at the top of each breath (half as much across)."},
    {L"effect breathe.period", L"Seconds for one breath."},
    {L"effect breathe.pivot", L"The point it grows from, as fractions of the box."},
    {L"effect wave.amplitude", L"How far it is pushed."},
    {L"effect wave.length", L"Distance from one crest to the next."},
    {L"effect wave.period", L"Seconds for a crest to travel one length."},
    {L"effect wave.direction", L"Which way the waves travel; the picture is pushed across their direction."},
    {L"effect rings.center", L"Where the ripples start, as fractions of the box."},
    {L"effect rings.amplitude", L"How far the ripples push."},
    {L"effect rings.length", L"Distance between ripples."},
    {L"effect rings.speed", L"Ripples passing a point a second."},
    {L"effect flow.direction", L"Direction of the flow in degrees: 0 right, 90 down."},
    {L"effect flow.speed", L"How fast the streaks run, in box heights a second."},
    {L"effect flow.amount", L"How far the streaks disturb the picture."},
    {L"effect pan.speed-x", L"Box heights a second the picture scrolls across; it wraps around within its box."},
    {L"effect pan.speed-y", L"Box heights a second the picture scrolls down."},
    {L"effect jitter.amount", L"How far it jumps."},
    {L"effect jitter.rate", L"Jumps a second."},
    {L"effect glow.strength", L"How bright the glow is."},
    {L"effect glow.radius", L"How far the glow spreads."},
    {L"effect glow.threshold", L"Only parts brighter than this glow (0 to 1)."},
    {L"effect glow.color", L"The glow's colour, multiplied with the colours that glow."},
    {L"effect rays.center", L"Where the light comes from, as fractions of the box."},
    {L"effect rays.strength", L"How bright the rays are."},
    {L"effect rays.length", L"How far the rays reach, as a share of the way to the centre."},
    {L"effect rays.color", L"The rays' colour."},
    {L"effect sheen.angle", L"The direction the band of light crosses, in degrees."},
    {L"effect sheen.width", L"The band's width."},
    {L"effect sheen.period", L"Seconds between sweeps; each sweep takes the first third."},
    {L"effect sheen.strength", L"How bright the band is."},
    {L"effect sheen.color", L"The band's colour."},
    {L"effect flicker.amount", L"How dark it can get (0 to 1)."},
    {L"effect flicker.rate", L"How fast it flickers."},
    {L"effect grade.brightness", L"Added to every colour."},
    {L"effect grade.contrast", L"Spreads colours away from middle grey (above 1) or towards it."},
    {L"effect grade.saturation", L"0 is grey; above 1 is more colourful."},
    {L"effect grade.temperature", L"Warmer (towards orange) above 0, cooler (towards blue) below."},
    {L"effect tint.color", L"The colour whose shades the picture takes."},
    {L"effect tint.amount", L"How fully it is tinted."},
    {L"effect blur.radius", L"How far it is blurred."},
    {L"effect haze.amount", L"How far the shimmer pushes the picture."},
    {L"effect haze.scale", L"The size of the shimmer's ripples."},
    {L"effect haze.speed", L"How fast the shimmer rises, in box heights a second."},
    {L"effect split.amount", L"How far red and blue are pulled apart."},
    {L"effect split.angle", L"The direction they are pulled, in degrees."},
    {L"effect grain.amount", L"How strong the grain is."},
    {L"effect grain.size", L"The size of a grain, in screen pixels."},
};

// What each built-in effect does.
const std::map<std::wstring, std::wstring> kEffects = {
    {L"bend", L"Sways back and forth, more the farther from its root: hair, a hem, grass. Paint the mask over what moves "
              L"and where it moves to."},
    {L"breathe", L"Grows a little about a point and back: breathing. Use a mask over the chest."},
    {L"wave", L"A sine wave travelling across or down it: a flag, a water surface."},
    {L"rings", L"Ripples spreading in circles from a point."},
    {L"flow", L"Streaks of disturbance running along a direction: a waterfall, drifting cloud."},
    {L"pan", L"Scrolls the picture within its box, wrapping around: a sky that drifts. The picture should tile."},
    {L"jitter", L"Jumps to a nearby place several times a second: a shake."},
    {L"glow", L"The bright parts, blurred and added as light. With a mask, only the masked bright parts glow; the light "
              L"spreads past the mask."},
    {L"rays", L"The bright parts streaked away from a centre and added as light: light shafts."},
    {L"sheen", L"A band of light sweeping across the opaque parts."},
    {L"flicker", L"Dims and recovers unevenly: a candle, a faulty lamp."},
    {L"grade", L"Brightness, contrast, saturation and warmth."},
    {L"tint", L"Turns the picture into shades of one colour."},
    {L"blur", L"Blurs it. With a mask, only the masked part."},
    {L"haze", L"A rising shimmer: heat haze."},
    {L"split", L"Pulls red and blue apart: chromatic aberration."},
    {L"grain", L"Film grain, fresh 24 times a second."},
};

// What each data name is.
const std::map<std::wstring, std::wstring> kData = {
    {L"time", L"Seconds since the scene started."},
    {L"clock.day", L"How far through the local day it is: 0 at midnight, 0.5 at noon."},
    {L"wake", L"0 at rest; rises to 1 while the lock screen fades away after the user wakes it."},
    {L"battery.level", L"The battery's charge, 0 to 1. On a PC without a battery it has no value."},
    {L"media.progress", L"How far through the playing track, 0 to 1."},
    {L"date.month", L"The month, 1 to 12."},
    {L"audio.level", L"How loud what the PC plays is, 0 to 1, smoothed."},
    {L"audio.bass", L"The same for the low notes."},
    {L"audio.mid", L"The same for the middle."},
    {L"audio.treble", L"The same for the high notes."},
    {L"clock.period", L"The part of the day by the clock: `dawn` 5 to 7, `day` 7 to 17, `dusk` 17 to 19, `night` otherwise."},
    {L"date.season", L"The season by the month, as north of the equator: `spring` March to May, and so on."},
    {L"battery.state", L"What the battery is doing."},
    {L"power.source", L"What the PC runs on."},
    {L"media.state", L"Whether something is playing."},
};

void Print(const std::wstring &s) { std::fwprintf(stdout, L"%s\n", s.c_str()); }

bool ReadFile(const fs::path &path, size_t limit, std::vector<uint8_t> *out) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > limit) return false;
    std::ifstream in(path, std::ios::binary);
    out->resize((size_t)size);
    return (bool)in.read((char *)out->data(), (std::streamsize)size) || size == 0;
}

// A picture's size from its PNG or JPEG header, without decoding it.
bool PictureSize(const std::vector<uint8_t> &b, uint32_t *w, uint32_t *h) {
    if (b.size() >= 24 && !memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) && !memcmp(b.data() + 12, "IHDR", 4)) {
        *w = (uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19];
        *h = (uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23];
        return *w && *h;
    }
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
    for (size_t i = 2; i + 9 < b.size();) {
        if (b[i] != 0xFF) { ++i; continue; }
        const uint8_t marker = b[i + 1];
        if (marker == 0xFF) { ++i; continue; }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
        const size_t length = (size_t)b[i + 2] << 8 | b[i + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            *h = (uint32_t)b[i + 5] << 8 | b[i + 6];
            *w = (uint32_t)b[i + 7] << 8 | b[i + 8];
            return *w && *h;
        }
        i += 2 + length;
    }
    return false;
}

// Video memory a w x h picture takes in the library: BC7 (a byte a pixel), BC4 for a mask (half a
// byte), four bytes a pixel kept exact; sides padded to whole 4 x 4 blocks; mipmaps add a third.
uint64_t TextureBytes(uint32_t w, uint32_t h, sg::AssetKind kind, bool exact) {
    // The importer scales pictures down to fit 8192 on their longer side.
    const double fit = std::min(1.0, 8192.0 / std::max(w, h));
    const uint64_t pw = ((uint64_t)std::max(1.0, w * fit) + 3) / 4 * 4, ph = ((uint64_t)std::max(1.0, h * fit) + 3) / 4 * 4;
    const double perPixel = exact ? 4 : kind == sg::AssetKind::Mask ? .5 : 1;
    return (uint64_t)(pw * ph * perPixel * 4 / 3);
}

// A scene's files by their names in the package.
struct Contents {
    std::map<std::wstring, std::vector<uint8_t>> files;
};

bool FromFolder(const fs::path &dir, Contents *c, std::vector<std::wstring> *problems) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::wstring name = fs::relative(it->path(), dir, ec).generic_wstring();
        std::vector<uint8_t> bytes;
        if (!ReadFile(it->path(), sg::kMaxTextureBytes, &bytes)) { problems->push_back(name + L": cannot be read, or too large"); continue; }
        c->files[name] = std::move(bytes);
    }
    if (ec) problems->push_back(L"cannot list " + dir.wstring());
    return problems->empty();
}

bool FromArchive(const fs::path &file, Contents *c, std::vector<std::wstring> *problems) {
    package::Reader reader;
    std::wstring why;
    if (!reader.Open(file.wstring(), &why)) {
        problems->push_back(why);
        if (reader.Compressed()) problems->push_back(L"an .alscene must be a zip of stored (uncompressed) entries");
        return false;
    }
    for (const package::Entry &e : reader.Entries()) {
        std::vector<uint8_t> bytes;
        if (!reader.Read(e, sg::kMaxTextureBytes, &bytes, &why)) { problems->push_back(FromUtf8(e.name) + L": " + why); continue; }
        c->files[FromUtf8(e.name)] = std::move(bytes);
    }
    return problems->empty();
}

struct Tally {
    int layers = 0, fills = 0, emitters = 0, groups = 0, effects = 0, shaderEffects = 0, motions = 0;
    double particles = 0;
};
void Count(const sg::Node &n, Tally *t) {
    switch (n.kind) {
    case sg::NodeKind::Layer: ++t->layers; break;
    case sg::NodeKind::Fill: ++t->fills; break;
    case sg::NodeKind::Group: ++t->groups; break;
    case sg::NodeKind::Particles: {
        ++t->emitters;
        double most = 2000;
        if (const auto *m = n.Find(L"max")) most = _wtof(m->c_str());
        else if (const auto *preset = n.Find(L"preset"))
            for (const auto &[p, name, value] : sg::PresetValues())
                if (p == *preset && name == L"max") most = _wtof(value.c_str());
        t->particles += most;
        break;
    }
    }
    for (const auto &e : n.effects) (e.source.empty() ? t->effects : t->shaderEffects) += 1;
    t->motions += (int)n.motions.size();
    for (const auto &c : n.children) Count(c, t);
}

std::wstring Megabytes(uint64_t bytes) { return Format(L"%.1f MB", bytes / 1048576.0); }

// Checks one scene; `problems` refuse it, `warnings` do not.
void CheckScene(const Contents &c, std::vector<std::wstring> *problems, std::vector<std::wstring> *warnings, std::vector<std::wstring> *summary) {
    const auto xmlFile = c.files.find(L"scene.xml");
    if (xmlFile == c.files.end()) { problems->push_back(L"scene.xml is missing"); return; }
    if (xmlFile->second.size() > 256 * 1024) { problems->push_back(L"scene.xml is larger than 256 KB"); return; }
    sg::Document d;
    std::wstring why;
    if (!sg::Parse(std::string_view((const char *)xmlFile->second.data(), xmlFile->second.size()), sg::Mode::Package, &d, &why)) {
        problems->push_back(L"scene.xml: " + why);
        return;
    }
    std::set<std::wstring> named{L"scene.xml"};
    uint64_t textures = 0;
    std::vector<std::pair<uint64_t, std::wstring>> largest;
    int shaders = 0, lotties = 0;
    for (const sg::Asset &a : d.assets) {
        const std::wstring line = Format(L"line %d: ", a.line);
        for (const std::wstring &f : a.files) {
            named.insert(f);
            const auto it = c.files.find(f);
            if (it == c.files.end()) { problems->push_back(line + f + L" is missing"); continue; }
            const auto &bytes = it->second;
            switch (a.kind) {
            case sg::AssetKind::Image: case sg::AssetKind::Mask: case sg::AssetKind::Frames: {
                uint64_t size = 0;
                uint32_t w = 0, h = 0;
                texture::Info info;
                if (f.size() > 4 && f.substr(f.size() - 4) == L".tex") {
                    if (!texture::Read(bytes.data(), bytes.size(), &info, nullptr, &why)) { problems->push_back(line + f + L": " + why); continue; }
                    size = bytes.size() - texture::kHeaderBytes;
                    w = info.usedWidth; h = info.usedHeight;
                } else if (!PictureSize(bytes, &w, &h)) {
                    problems->push_back(line + f + L" is not a PNG or JPEG picture");
                    continue;
                } else size = TextureBytes(w, h, a.kind, a.exact);
                textures += size;
                largest.push_back({size, f + Format(L" (%u x %u)", w, h)});
                if (w > 2u * d.width && h > 2u * d.height)
                    warnings->push_back(line + f + Format(L" is %u x %u, more than twice the stage both ways: unless it is scaled up, a "
                                                          L"smaller picture looks the same and takes less memory", w, h));
                break;
            }
            case sg::AssetKind::LayerShader: case sg::AssetKind::EffectShader: {
                ++shaders;
                std::vector<uint8_t> bytecode;
                if (bytes.size() > sg::kMaxShaderBytes) problems->push_back(line + f + L" is larger than 64 KB");
                else if (!scene::CompileShader(std::string_view((const char *)bytes.data(), bytes.size()), &bytecode, &why))
                    problems->push_back(line + f + L": " + why);
                break;
            }
            case sg::AssetKind::Lottie:
                ++lotties;
                if (bytes.size() > sg::kMaxLottieBytes) problems->push_back(line + f + L" is larger than 16 MB");
                break;
            case sg::AssetKind::Video: break;
            }
        }
    }
    for (const auto &[name, bytes] : c.files)
        if (!named.count(name) && name != L"still.bmp" && name != L"poster.bmp") warnings->push_back(name + L" is not used by scene.xml and is left out when imported");
    if (textures > sg::kMaxTextureBytes)
        problems->push_back(L"the pictures take " + Megabytes(textures) + L" of video memory once compressed; a scene may take at most 512 MB");
    else if (textures > 300ull * 1024 * 1024)
        warnings->push_back(L"the pictures take " + Megabytes(textures) + L" of video memory once compressed: over 300 MB, a PC with "
                            L"little video memory shows them less sharp");
    // Nodes that fill the stage at the top level move with a camera unless they say depth="0".
    if (!d.camera.empty())
        for (const sg::Node &n : d.nodes)
            if (n.kind == sg::NodeKind::Fill && !n.Find(L"depth") && !n.Find(L"width") && !n.Find(L"height") && !n.Find(L"space"))
                warnings->push_back(Format(L"line %d: this fill covers the stage and moves with the camera, so an edge of the screen shows "
                                           L"past it; an overlay should say depth=\"0\"", n.line));
    Tally t;
    for (const sg::Node &n : d.nodes) Count(n, &t);
    summary->push_back(Format(L"\"%s\": stage %d x %d, still picture at %g s", d.name.c_str(), d.width, d.height, d.still));
    std::wstring holds;
    const auto count = [&](int n, const wchar_t *one, const wchar_t *many) {
        if (n) holds += (holds.empty() ? L"" : L", ") + Format(L"%d %s", n, n == 1 ? one : many);
    };
    count(t.layers, L"layer", L"layers");
    count(t.fills, L"fill", L"fills");
    count(t.groups, L"group", L"groups");
    count(t.emitters, L"particle emitter", L"particle emitters");
    if (t.emitters) holds += Format(L" (up to %.0f particles)", t.particles);
    count(t.effects, L"effect", L"effects");
    count(t.shaderEffects, L"effect shader", L"effect shaders");
    count(t.motions, L"motion", L"motions");
    count((int)d.settings.size(), L"setting", L"settings");
    if (!d.camera.empty()) holds += L", a moving camera";
    summary->push_back(holds);
    std::sort(largest.rbegin(), largest.rend());
    std::wstring big;
    for (size_t i = 0; i < largest.size() && i < 3; ++i) big += (i ? L", " : L"; largest: ") + largest[i].second + L" " + Megabytes(largest[i].first);
    if (!largest.empty())
        summary->push_back(Format(largest.size() == 1 ? L"%zu picture file takes %s of video memory once compressed"
                                                      : L"%zu picture files take %s of video memory once compressed",
                                  largest.size(), Megabytes(textures).c_str()) + big);
    if (shaders) summary->push_back(Format(shaders == 1 ? L"%d shader compiled to the contract" : L"%d shaders compiled to the contract", shaders));
    if (lotties) summary->push_back(Format(lotties == 1 ? L"%d Lottie animation, checked when imported" : L"%d Lottie animations, checked when imported", lotties));
}

int Check(const std::vector<fs::path> &paths);

int Pack(const fs::path &dir, const fs::path &out) {
    if (Check({dir})) return 1;
    std::vector<package::Item> items;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) items.push_back({ToUtf8(fs::relative(it->path(), dir, ec).generic_wstring()), it->path().wstring(), {}});
    std::wstring why;
    if (ec || !package::Write(items, fs::absolute(out).wstring(), &why)) {
        Print(L"cannot write " + out.wstring() + L": " + why);
        return 1;
    }
    Print(L"wrote " + out.wstring());
    return 0;
}

int Check(const std::vector<fs::path> &paths) {
    int failed = 0;
    for (const fs::path &p : paths) {
        Contents c;
        std::vector<std::wstring> problems, warnings, summary;
        std::error_code ec;
        if (fs::is_directory(p, ec)) FromFolder(p, &c, &problems);
        else if (p.extension() == L".alscene") FromArchive(p, &c, &problems);
        else if (p.filename() == L"scene.xml") FromFolder(p.parent_path(), &c, &problems);
        else problems.push_back(L"expected a scene folder, its scene.xml, or an .alscene");
        if (problems.empty()) CheckScene(c, &problems, &warnings, &summary);
        Print(p.wstring() + (problems.empty() ? L": accepted" : L": refused"));
        for (const auto &s : summary) Print(L"  " + s);
        for (const auto &w : warnings) Print(L"  warning: " + w);
        for (const auto &e : problems) Print(L"  error: " + e);
        failed += !problems.empty();
    }
    return failed ? 1 : 0;
}

std::wstring Codes(const std::vector<std::wstring> &words) {
    std::wstring s;
    for (const auto &w : words) s += (s.empty() ? L"`" : L" `") + w + L"`";
    return s;
}

// The reference, or false with `missing` naming what has no description.
bool Reference(std::wstring *out, std::vector<std::wstring> *missing) {
    std::wstring s;
    s += L"<!-- Generated by tools/scene_check from the scene reader. Do not edit: after changing\n"
         L"     common/src/scenegraph.cpp, build and run\n"
         L"     scene_check --write-reference skills/animelogon-scenes/reference/attributes.md -->\n\n";
    s += L"# Scene attributes\n\n";
    s += L"Every attribute of the nodes, particles and built-in effects of a scene, with the values AnimeLogon accepts. "
         L"The scene file's other elements are in [scene-format.md](scene-format.md). Any value may instead be `$id`, the "
         L"value of one of the scene's settings.\n\n";
    s += L"- **Default**: the value when the attribute is not written.\n";
    s += L"- **Moves**: whether a motion (`<keys>`, `<wave>`, `<wander>`, `<drive>`) can move it. An effect's attribute is "
         L"moved as `<effect id>.<attribute>`, so the effect needs an `id`.\n";
    s += L"- Distances in effects are fractions of the node's box height unless said otherwise.\n";
    std::wstring element;
    for (const sg::AttributeInfo &a : sg::AttributeTable()) {
        if (a.element != element) {
            element = a.element;
            if (element.rfind(L"effect ", 0) == 0) {
                const std::wstring type = element.substr(7);
                if (type == sg::EffectTypes().front()) s += L"\n## Built-in effects\n\nWritten as `<effect type=\"...\">` inside a node.\n";
                s += L"\n### `" + type + L"`\n\n";
                const auto about = kEffects.find(type);
                if (about == kEffects.end()) missing->push_back(L"effect " + type);
                else s += about->second + L"\n\n";
            } else {
                s += element == L"node" ? L"\n## Every node\n\n`<layer>`, `<fill>`, `<particles>` and `<group>` all take these.\n\n"
                                        : L"\n## `<" + element + L">`\n\n";
            }
            s += L"| Attribute | Value | Default | Moves | Description |\n";
            s += L"|---|---|---|---|---|\n";
        }
        const auto about = kAbout.find(a.element + L"." + a.name);
        if (about == kAbout.end()) missing->push_back(L"attribute " + a.element + L"." + a.name);
        s += L"| `" + a.name + L"` | " + a.values + L" | " + (a.fallback.empty() ? L"—" : L"`" + a.fallback + L"`") + L" | " +
             (a.animated ? L"yes" : L"no") + L" | " + (about == kAbout.end() ? L"" : about->second) + L" |\n";
    }

    s += L"\n## Particle presets\n\nWhat each `preset` writes for the attributes the element leaves out.\n\n";
    s += L"| Preset | Values |\n|---|---|\n";
    std::map<std::wstring, std::wstring> presets;
    std::vector<std::wstring> order;
    for (const auto &[preset, name, value] : sg::PresetValues()) {
        if (!presets.count(preset)) order.push_back(preset);
        presets[preset] += (presets[preset].empty() ? L"" : L" ") + name + L"=\"" + value + L"\"";
    }
    for (const auto &p : order) s += L"| `" + p + L"` | `" + presets[p] + L"` |\n";

    s += L"\n## Data\n\nNames a `when` condition and a `<drive by>` can read. Numbers drive a property; states are compared "
         L"with `=` or `!=`.\n\n";
    s += L"| Name | Values | Description |\n|---|---|---|\n";
    for (const sg::DataInfo &v : sg::DataNames()) {
        const auto about = kData.find(v.name);
        if (about == kData.end()) missing->push_back(L"data " + v.name);
        s += L"| `" + v.name + L"` | " + (v.words.empty() ? std::wstring(L"number") : Codes(v.words)) + L" | " +
             (about == kData.end() ? L"" : about->second) + L" |\n";
    }

    s += L"\n## Limits\n\n| What | At most |\n|---|---|\n";
    s += L"| `scene.xml` | 256 KB, UTF-8 |\n";
    s += Format(L"| Nodes, groups included | %zu |\n", sg::kMaxNodes);
    s += Format(L"| Group nesting | %zu levels |\n", sg::kMaxNesting);
    s += Format(L"| Effects on one node | %zu |\n", sg::kMaxEffectsPerNode);
    s += Format(L"| Effects in the scene | %zu |\n", sg::kMaxEffects);
    s += Format(L"| Particle emitters | %zu |\n", sg::kMaxEmitters);
    s += Format(L"| Particles, all emitters' `max` together | %zu |\n", sg::kMaxParticles);
    s += Format(L"| Pictures and masks (a frame sequence counts once) | %zu |\n", sg::kMaxImages);
    s += Format(L"| Frames in one sequence | %zu |\n", sg::kMaxFrames);
    s += Format(L"| Videos | %zu |\n", sg::kMaxVideos);
    s += Format(L"| Lottie animations | %zu, %zu MB each |\n", sg::kMaxLotties, sg::kMaxLottieBytes >> 20);
    s += Format(L"| Shaders | %zu, %zu KB of source each |\n", sg::kMaxShaders, sg::kMaxShaderBytes >> 10);
    s += Format(L"| Settings | %zu |\n", sg::kMaxSettings);
    s += Format(L"| Motions on one node | %zu |\n", sg::kMaxMotionsPerNode);
    s += Format(L"| Keys in one `<keys>` | %zu |\n", sg::kMaxKeys);
    s += Format(L"| Video memory of all pictures once compressed | %zu MB (300 MB or less is best) |\n", sg::kMaxTextureBytes >> 20);
    s += L"| A picture's longer side | 8192 pixels; larger ones are scaled down when imported |\n";
    *out = std::move(s);
    return missing->empty();
}

int WriteReference(const fs::path &file) {
    std::wstring text;
    std::vector<std::wstring> missing;
    if (!Reference(&text, &missing)) {
        for (const auto &m : missing) Print(L"no description for " + m + L" in tools/scene_check.cpp");
        return 1;
    }
    std::wstring crlf;
    for (wchar_t c : text) crlf += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
    const std::string bytes = ToUtf8(crlf);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), (std::streamsize)bytes.size());
    if (!out) { Print(L"cannot write " + file.wstring()); return 1; }
    Print(L"wrote " + file.wstring());
    return 0;
}

int CheckReference(const fs::path &file) {
    std::wstring text;
    std::vector<std::wstring> missing;
    const bool described = Reference(&text, &missing);
    for (const auto &m : missing) Print(L"no description for " + m + L" in tools/scene_check.cpp");
    std::vector<uint8_t> bytes;
    if (!ReadFile(file, 1 << 20, &bytes)) { Print(L"cannot read " + file.wstring()); return 1; }
    std::wstring found;
    for (wchar_t c : FromUtf8(std::string_view((const char *)bytes.data(), bytes.size())))
        if (c != L'\r') found += c;
    if (!described) return 1;
    if (found != text) {
        Print(file.wstring() + L" is out of date: run scene_check --write-reference " + file.wstring());
        return 1;
    }
    Print(file.wstring() + L" is up to date");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc == 3 && std::wstring(argv[1]) == L"--write-reference") return WriteReference(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--check-reference") return CheckReference(argv[2]);
    if (argc == 4 && std::wstring(argv[1]) == L"--pack") return Pack(argv[2], argv[3]);
    if (argc < 2 || argv[1][0] == L'-') {
        std::fwprintf(stderr, L"usage: scene_check <scene folder|scene.xml|.alscene>...\n"
                              L"       scene_check --pack <folder> <out.alscene>\n"
                              L"       scene_check --write-reference <file> | --check-reference <file>\n");
        return 2;
    }
    std::vector<fs::path> paths(argv + 1, argv + argc);
    return Check(paths);
}
