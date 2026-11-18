// scene.exe's drawing: a stored scene's node tree, composited on the graphics card. Runs only in the
// sandboxed renderer process; the parent never parses a scene's content files.
#pragma once
#include <d3d11.h>
#include <memory>
#include <string_view>
#include "animelogon/scenehost.h"
namespace animelogon::scene {
// The shader contract (D51): ps_5_0 returning one float4 SV_Target, taking at most a float2
// TEXCOORD0, and binding only the Scene constant block, the `source` and `mask` textures and the
// linearClamp sampler. Compiling prepends the declarations an author's file relies on.
bool CompileShader(std::string_view source, std::vector<uint8_t> *bytecode, std::wstring *why);
bool CheckShader(const uint8_t *bytes, size_t size, std::wstring *why);

class Renderer {
public:
    Renderer();
    ~Renderer();
    // `files` are a stored scene's library files. Its textures are kept within `videoMemory` MB by
    // leaving out the largest ones' finest mipmap levels; 0 chooses a limit from the graphics card.
    bool Open(ID3D11Device *device, const Files &files, int width, int height, int videoMemory, std::wstring *why);
    // Draws the scene at `inputs` into `output` (width x height), working at `scale` of that
    // resolution. `video` holds the newest video frame, or is null before the first one.
    bool Draw(const Inputs &inputs, ID3D11Texture2D *video, ID3D11Texture2D *output, float scale, float *milliseconds, std::wstring *why);
    // The user's quality (settings.h): how coarse the resolution of blurs, glows and rays may be.
    void SetQuality(RenderQuality quality);
    // The user's particle density, 10..100 percent of what each emitter would give off.
    void SetParticles(int percent);
    // How long the trial plays to see the scene's motion.
    float TrialSeconds() const;
    uint64_t TextureBytes() const;
    uint32_t ReducedLevels() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};

// Turns a package's files (scene.xml, pictures, Lottie files, shader sources, and poster.bmp for a
// video's first frame) into the library's: textures, checked Lottie files, shader sources with their
// compiled forms, and scene.xml naming them.
bool Import(ID3D11Device *device, const Files &package, Files *library, std::wstring *why);
}  // namespace animelogon::scene
