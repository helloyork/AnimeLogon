// A scene's particles, drawn on the graphics card. Every particle's place, size and colour at a
// moment come from a closed formula of the time, its number and the emitter's seed, so any moment
// can be drawn directly and nothing depends on the frame rate. Runs only in the sandboxed renderer.
#pragma once
#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>
#include "animelogon/scenegraph.h"

namespace animelogon::scene {

// A picture particles wear: a texture and the part of it in use.
struct ParticlePicture {
    ID3D11ShaderResourceView *view = nullptr;
    float u = 1, v = 1;
    bool premultiplied = true;
};

// Where one emitter's particles land: its box (boxWidth x boxHeight in its units) carried onto a
// target's pixels by an affine map, through a blend state, the node's opacity and its mask.
struct ParticleTarget {
    ID3D11RenderTargetView *target = nullptr;
    int width = 0, height = 0;
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e, y' = b x + d y + f
    double boxWidth = 0, boxHeight = 0;
    ID3D11BlendState *blend = nullptr;
    float opacity = 1;
    ID3D11ShaderResourceView *mask = nullptr;
    float maskU = 1, maskV = 1;
};

class ParticleDrawer {
public:
    ParticleDrawer();
    ~ParticleDrawer();
    bool Open(ID3D11Device *device, std::wstring *why);
    // Draws emitter `p` as it is at `time`. `frames` is its picture (one per frame of a sequence) or
    // empty for a soft dot or streak; `density` (0..1) scales how many particles are born.
    void Draw(const scenegraph::Placed &p, const std::vector<ParticlePicture> &frames, const ParticleTarget &target, double time, double density);
private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace animelogon::scene
