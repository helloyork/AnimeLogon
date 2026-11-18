#pragma once
#include <d3d11.h>
#include <memory>
#include <functional>
#include <wrl/client.h>
#include "animelogon/scenegraph.h"
#include "animelogon/settings.h"

namespace animelogon::scene {
using scenegraph::File;
using scenegraph::Files;
struct Inputs {
    LONG sequence = 0;
    LONG videoSequence = 0;
    scenegraph::Inputs data;
    scenegraph::Values values;
};
// Where the automatic quality starts: what the import trial measured.
struct Tier {
    bool calibrated = false;
    int fps = 60;  // 60: as the cap allows; 30; 0: the still picture
    float scale = 1;
};
// How a renderer is started.
struct Options {
    // Importing turns a package's files into the library's (textures, checked Lottie files,
    // compiled shaders), then measures the trial; otherwise the files are the library's.
    bool importing = false;
    Tier tier;
    Inputs initial;
    RenderQuality quality = RenderQuality::Auto;
    int videoMemory = 0;  // MB; 0 is chosen from the graphics card
    int particles = kParticlesMax;
};
class Host {
public:
    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    bool Start(ID3D11Device *device, const Files &files, int width, int height, const Options &options, std::wstring *why);
    void Stop();
    // `fps` is the frame rate cap for now (settings.h FrameRateNow, with `display` made the
    // display's own rate).
    LONG SetRunning(bool running, const Inputs &inputs, float fps = 60, bool redraw = false);
    LONG FrameSequence() const;
    // Nonblocking. Copies completed frames to a texture that the child cannot access.
    bool Poll();
    bool Ready() const;
    bool Failed(std::wstring *why) const;
    ID3D11ShaderResourceView *View() const;
    int width() const;
    int height() const;
    // Import results and the frame at the initial inputs, after Ready().
    bool Normalized(Files *files, std::wstring *why) const;
    std::vector<uint8_t> Bitmap(std::wstring *why) const;
    float Milliseconds() const;
    bool Profile(Tier *tier) const;
    uint64_t TextureBytes() const;
    // Runs trusted conversion/copy while holding the input texture's keyed mutex.
    bool UpdateVideo(const std::function<void(ID3D11Texture2D *)> &draw);
private:
    struct State;
    std::unique_ptr<State> s_;
};
// Reads an installed (or imported) scene's library files: scene.xml and what LibraryFiles names,
// but not the video, which the parent decodes and hands over frame by frame.
bool LoadFiles(const std::wstring &directory, bool trusted, Files *files, scenegraph::Document *document, std::wstring *why);
bool Device(Microsoft::WRL::ComPtr<ID3D11Device> *device, std::wstring *why);
}  // namespace animelogon::scene
namespace animelogon::data { struct Snapshot; }
namespace animelogon::scene {
// Fills the clock, date, battery and media parts of `inputs` from the local time and a data
// snapshot; `now` is UTC FILETIME ticks, for the media position.
void SetData(scenegraph::Inputs *inputs, const data::Snapshot &snapshot, uint64_t now);
}  // namespace animelogon::scene
