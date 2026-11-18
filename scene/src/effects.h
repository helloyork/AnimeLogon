// The effects a scene node can carry: our built-in ones and authors' effect shaders, run in order
// over the node's own picture before it is composited. Runs only in the sandboxed renderer.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "animelogon/scenegraph.h"

namespace animelogon::scene {

// What an author's shader can read (D51). A layer shader draws from nothing but these numbers; an
// effect shader also reads `source`, the node's picture so far within its box (premultiplied), and
// `mask`, the effect's mask over the box (white everywhere when the effect has none).
constexpr char kShaderHeader[] = R"(
cbuffer Scene : register(b0) {
    float time; float2 resolution; float day;
    float battery; float progress; float playing; float wake;
    float4 audio;
    float month; float3 reserved;
    float4 param[16];
};
Texture2D<float4> source : register(t0);
Texture2D<float4> mask : register(t1);
SamplerState linearClamp : register(s0);
)";
struct ShaderConstants {
    float time = 0, resolution[2]{}, day = 0;
    float battery = -1, progress = -1, playing = 0, wake = 0;
    float audio[4]{};
    float month = 1, reserved[3]{};
    float param[16][4]{};
};
// The constants for an author's shader: the inputs at `time`, a `width` x `height` target and the
// slots in `values` whose names are "<prefix>0".."<prefix>15".
ShaderConstants ShaderInputs(const scenegraph::Inputs &data, double time, int width, int height,
                             const std::map<std::wstring, std::array<float, 4>> &values, const std::wstring &prefix);

// An offscreen picture. Its texture can be larger than the part in use, so that a picture whose
// size changes from frame to frame does not need a new texture every frame.
struct Surface {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    int width = 0, height = 0;  // the texture's size
    // At least w x h; grown in steps of 64 pixels when `room`, otherwise exactly w x h.
    bool Fit(ID3D11Device *device, int w, int h, bool room = true);
};

// A mask texture and the part of it the picture uses.
struct MaskView {
    ID3D11ShaderResourceView *view = nullptr;
    float u = 1, v = 1;
};

// Where a node's picture is: `width` x `height` pixels with the box `margin` pixels in from each
// edge. `extend` repeats the box's edge pixels outward instead of reading transparency there, for
// pictures meant to cover the screen.
struct EffectBox {
    int width = 0, height = 0, margin = 0;
    bool extend = false;
    int Used(bool across) const { return (across ? width : height) + 2 * margin; }
};

// The largest side of a node's effect picture, margin included.
constexpr int kMaxEffectPicture = 8192;

class EffectChain {
public:
    EffectChain();
    ~EffectChain();
    bool Open(ID3D11Device *device, std::wstring *why);
    // How many pixels the effects can move or spread a w x h box's picture outward.
    static int Margin(const std::vector<scenegraph::PlacedEffect> &effects, int width, int height);
    // Runs `effects` in order over the picture in `a`; `b` is a scratch surface at least as large.
    // `soft` (1, 2 or 4) is the coarsest resolution divisor blurs, glows and rays may work at.
    // `*result` is set to whichever of a and b holds the outcome.
    bool Run(const std::vector<scenegraph::PlacedEffect> &effects, const EffectBox &box, Surface *a, Surface *b,
             const std::map<std::wstring, MaskView> &masks, const std::map<std::wstring, Microsoft::WRL::ComPtr<ID3D11PixelShader>> &shaders,
             const scenegraph::Inputs &data, int soft, Surface **result, std::wstring *why);
    // Lets go of scratch textures, as when the canvas changes size.
    void Trim();
private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace animelogon::scene
