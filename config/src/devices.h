// Listing the audio output devices, for the sound page.
#pragma once

#include <string>
#include <vector>

namespace devices {

struct Output {
    std::wstring id;
    std::wstring name;
};

// Active render endpoints. COM must be initialised on the calling thread.
std::vector<Output> ListOutputs();

}  // namespace devices
