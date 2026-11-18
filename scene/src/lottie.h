// A Lottie animation drawn on the processor by ThorVG, inside the sandboxed renderer only.
#pragma once
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace animelogon::scene {
class Lottie {
public:
    Lottie();
    ~Lottie();
    // Checks the JSON (no expressions, no external resources, bounded structure) and loads it.
    bool Open(const std::vector<uint8_t> &bytes, std::wstring *why);
    // Whether the file has a slot with this id that a value can be given to.
    bool HasSlot(const std::wstring &id) const;
    // Draws the frame at `time` seconds (looping) into width x height premultiplied BGRA pixels,
    // with the slots given values. Returns false on failure; `changed` says whether the pixels differ
    // from the last call's.
    bool Draw(const std::map<std::wstring, std::array<float, 4>> &slots, double time, int width, int height,
              bool *changed, std::wstring *why);
    const uint8_t *Pixels() const;
    float Width() const;
    float Height() const;
    float Duration() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}  // namespace animelogon::scene
