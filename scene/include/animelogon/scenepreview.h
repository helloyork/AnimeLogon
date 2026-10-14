#pragma once
#include "animelogon/scenehost.h"
#include "animelogon/wallpaper.h"
namespace animelogon::scene {
// User-side preview and author tool. All author shaders and Lottie stay in Host's child.
class Preview {
public:
    Preview();
    ~Preview();
    bool Open(const WallpaperInfo &wallpaper, int width, int height, const skin::Values &values, std::wstring *why, bool author = false);
    void SetValues(const skin::Values &values);
    bool Frame(const Inputs &inputs, bool running, std::vector<uint8_t> *bitmap, std::wstring *why);
    bool Failed(std::wstring *why) const;
    std::vector<std::array<float,4>> Bounds() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
