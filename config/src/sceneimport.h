#pragma once
#include "staging.h"
#include "animelogon/package.h"
namespace staging {
Result SceneWallpaper(animelogon::package::Reader &reader, const std::wstring &dir, const std::wstring &name,
                      const std::atomic<bool> &cancel, const Progress &progress);
Result Scene(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel, const Progress &progress);
// The same import for authors' tools (tools/scene_preview), which preview a scene whether or not the
// settings app takes scenes yet (kTakesScenes).
Result SceneForAuthors(const std::wstring &source, const std::wstring &dir, const std::atomic<bool> &cancel, const Progress &progress);
}
