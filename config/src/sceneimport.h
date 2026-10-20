#pragma once
#include "staging.h"
#include "animelogon/package.h"
namespace staging {
Result SceneWallpaper(animelogon::package::Reader &reader, const std::wstring &dir, const std::wstring &name,
                      const std::atomic<bool> &cancel, const Progress &progress);
Result Scene(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel, const Progress &progress);
}
