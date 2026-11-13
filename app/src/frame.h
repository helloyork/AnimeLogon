// The settings window without Windows' title bar. The page's own bar (app/web, 48 px) is the top
// of the window and carries the window's buttons at its right end; this keeps everything else
// Windows gives a window through its frame: the shadow and rounded corners, resizing from every
// edge, Aero Snap and Snap Layouts, the system menu, and the minimise and maximise animations.
// The window's border and the colour Windows would give its caption are neutral greys that follow
// the page's light or dark scale, so the person's accent colour never shows on the window.
#pragma once

#include <windows.h>

#include <functional>

#include "json.h"

namespace app {

class Frame {
public:
    using Sender = std::function<void(const json::Writer &)>;
    explicit Frame(Sender send) : send_(std::move(send)) {}

    // Right after the window is created: its colours and corners, the two edge windows, and its
    // frame worked out again without the caption.
    void Attach(HWND window, bool dark);
    // Light or dark, as Windows' setting for apps is.
    void Colours(bool dark);
    // What the page draws behind everything (tokens.css --bg), for whatever shows before it does.
    static COLORREF Background(bool dark);

    // The window's messages about its frame. Returns true with the answer in `*result` for those
    // it answers; the window goes on to handle the rest, including some this only takes note of.
    bool Handle(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);
    // WebView2 puts windows of its own over the client area (the page and its drag regions): the
    // edge windows go back on top of them.
    void Raise();
    // The page's messages about the window (`op` frame or window). Returns false for any other.
    bool OnPage(const json::Value &m);
    // Tells the page the window's title, whether it is maximised and active, and how its maximise
    // button is: as these change, and after the title is set again in another language.
    void Tell();

private:
    static LRESULT CALLBACK EdgeProc(HWND edge, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Edge(HWND edge, UINT msg, WPARAM wp, LPARAM lp);
    // The edge windows where they belong for the window's size and state.
    void Place();
    // How far into the window from its top edge it resizes, at the window's DPI.
    int Border() const;

    HWND window_ = nullptr;
    // Over the page, never drawn: the strip along the top edge that resizes the window while it is
    // not maximised, and the page's maximise button, so Windows can offer Snap Layouts there.
    HWND top_ = nullptr, max_ = nullptr;
    // The page's maximise button in client pixels, as it last said, with `left` and `right` the
    // distances of its edges from the client area's right edge; empty when it has none.
    RECT maxButton_{};
    // Inactive until Windows says otherwise, which it does when it shows the window activated.
    bool dark_ = false, active_ = false, maximised_ = false;
    bool hover_ = false, press_ = false, tracking_ = false;
    Sender send_;
};

}  // namespace app
