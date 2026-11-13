// AnimeLogon's app id on Steam, used when animelogon-steam.exe is started without --appid.
// Zero until Steam has given one: the helper then answers every request with "no_appid".
#pragma once

#include <cstdint>

namespace steamhelper {

constexpr uint32_t kAppId = 0;

}  // namespace steamhelper
