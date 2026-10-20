#pragma once
#include <memory>
#include <vector>
#include "animelogon/scenehost.h"
namespace animelogon::scene {
class Lottie {
public:
    Lottie();
    ~Lottie();
    bool Open(const File &file, const Document &document, const Layer &layer, std::wstring *why);
    bool Draw(const Document &document, const Layer &layer, const Inputs &inputs, int width, int height, std::wstring *why);
    const uint8_t *Pixels() const;
    float Aspect() const;
    float Duration() const;
private:
    struct State;
    std::unique_ptr<State> s_;
};
}
