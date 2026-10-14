#pragma once
#include <d3d11.h>
#include <memory>
#include <functional>
#include <wrl/client.h>
#include "animelogon/scene.h"

namespace animelogon::scene {
struct Inputs {
    float time = 0, day = 0, battery = -1, progress = 0, playing = 0;
    skin::Values values;
};
struct Tier {
    bool calibrated = false;
    int fps = 60;
    float scale = 1;
};
class Host {
public:
    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    bool Start(ID3D11Device *device, const Files &files, int width, int height, bool importing, std::wstring *why, const Tier &tier = {});
    void Stop();
    void SetRunning(bool running, const Inputs &inputs, float refresh = 60);
    // Nonblocking. Copies completed frames to a texture that the child cannot access.
    bool Poll();
    bool Ready() const;
    bool Failed(std::wstring *why) const;
    ID3D11ShaderResourceView *View() const;
    int width() const;
    int height() const;
    // Import results and the frame at time zero, after Ready().
    bool Normalized(Files *files, std::wstring *why) const;
    std::vector<uint8_t> Bitmap(std::wstring *why) const;
    float Milliseconds() const;
    bool Profile(Tier *tier) const;
    // Runs trusted conversion/copy while holding the input texture's keyed mutex.
    bool UpdateVideo(const std::function<void(ID3D11Texture2D *)> &draw);
private:
    struct State;
    std::unique_ptr<State> s_;
};
bool LoadFiles(const std::wstring &directory, bool trusted, Files *files, Document *document, std::wstring *why);
bool Device(Microsoft::WRL::ComPtr<ID3D11Device> *device, std::wstring *why);
}  // namespace animelogon::scene
