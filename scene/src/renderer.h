#pragma once
#include <d3d11.h>
#include <memory>
#include "animelogon/scenehost.h"
namespace animelogon::scene {
class Renderer {
public:
    Renderer();
    ~Renderer();
    bool Open(ID3D11Device *device, Files files, bool importing, int width, int height, std::wstring *why);
    bool Draw(const Inputs &inputs, ID3D11Texture2D *video, ID3D11Texture2D *output, float scale, float *milliseconds, std::wstring *why);
    const Files &Normalized() const;
    float TrialSeconds() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
