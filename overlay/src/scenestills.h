// A scene shows a still before its first frame, whenever it cannot be drawn, on the other displays
// after a wake, and in the sign-in background baked from it (bake.h). A scene's still.bmp is drawn
// at import with the scene's default settings, so for a theme that gives them values the overlay
// draws another with them: the same size as still.bmp, at the same moment of the scene, at the
// highest quality, in the sandboxed renderer like any frame of the scene. It is kept in the library
// (wallpaper.h SceneStillPath), where the plan takes it in place of still.bmp (plan.h).
//
// Stills are drawn while the person at the console uses their desktop, like the bake that waits for
// them, and a lock stops one: the lock's own scene comes first, and the still is drawn afresh the
// next time the desktop is in use.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <memory>
#include <set>
#include <string>

#include "plan.h"

namespace animelogon::scene {
class Host;
}

// The first wallpaper in `plan` whose own still is wanted and is not drawn, leaving out those whose
// still could not be drawn (`failed`, by ownStill); null if there is none.
const plan::Wallpaper *StillToDraw(const plan::Plan &plan, const std::set<std::wstring> &failed);

class SceneStills {
public:
    SceneStills();
    ~SceneStills();
    SceneStills(const SceneStills &) = delete;
    SceneStills &operator=(const SceneStills &) = delete;

    // Starts drawing the first still `plan` wants, unless it is being drawn already. True while a
    // still is wanted and being drawn, so that the bake waits for it; false when every one there is,
    // or could not be drawn, and the plan shows still.bmp in its place.
    bool Want(const plan::Plan &plan, ID3D11Device *device);
    // Moves the drawing on. Without `allowed` (a lock, the desktop left) it stops. True when a still
    // has just been written, so that the background can be baked from it at once.
    bool Tick(bool allowed);

private:
    void Fail(const std::wstring &why);

    std::unique_ptr<animelogon::scene::Host> host_;
    std::wstring path_, id_;
    ULONGLONG started_ = 0;
    std::set<std::wstring> failed_;
    bool pruned_ = false;
};
