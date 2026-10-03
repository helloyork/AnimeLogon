// Micula / drop_down.h

#pragma once

#include "scroll_bar.h"
#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace micula {


struct DropDown : Widget {
    std::vector<std::wstring> options;
    int selected = 0;
    std::function<void(int)> onChange;
    bool open = false;
    float rowH = 32.0f;
    // Whether the choice is a ring. Off, the list has two ends, and a step past one of them
    // has nowhere to go -- which is what the knock is for. On, the step past the last option
    // arrives at the first: the wheel's, Up and Down's, and either of them while the list is
    // closed as well as open, because a control whose keys and wheel disagree about its ends
    // is a control with two answers. The list's own scroll bar is not part of this: a bar has
    // two ends by definition, and so does the room the popup is shown through.
    bool wrapAround = false;
    // How far the lid is open: 0 is the control's own row and nothing else, 1 is the whole
    // popup. Fluent expands a flyout out of the control it belongs to rather than blinking
    // it on, and on the way out it collapses back into it rather than vanishing -- which is
    // why the control stays raised while this runs down.
    //
    // Two curves, because Fluent has two: in on "Fast Out, Slow In", out on "Slow Out,
    // Fast In". A flyout arriving settles; a flyout leaving gets out of the way.
    motion::Track openT;

    DropDown(std::vector<std::wstring> opts, int sel, std::function<void(int)> f)
        : options(std::move(opts)), selected(sel), onChange(std::move(f)) {}
    // A list thrown away while open -- the page laid out again under it -- must not leave
    // its bar's timers firing at the window with nobody to answer them.
    ~DropDown() override {
        // The list's own scroll bar goes with it, and its timers go with the bar: see Timer.
    }

    // The control's own row. While the list is open `rect` grows to cover the popup, so
    // the head cannot be derived from `rect` any more.
    D2D1_RECT_F head = {};
    // The rows' margin inside the popup, top and bottom. WinUI's
    // ComboBoxDropdownContentMargin, of which the 4 DIPs that show are the part that
    // matters here.
    static constexpr float kPad = 4.0f;
    // Where the popup comes to rest: worked out when it opens, from the chosen row, and
    // then left alone -- the popup does not move while it is being scrolled through, the
    // *rows* do. See Place().
    D2D1_RECT_F frame = {};
    // The list's own offset for a choice: the chosen row on the control's own row, as far as
    // the list allows. The offset cannot leave the range the bar has -- the first row at the
    // panel's top and the last at its bottom -- because outside that range the panel would be
    // showing rows that do not exist, which is blank space. A choice within a window's height
    // of either end therefore stops there, and the chosen row comes to rest as the panel's
    // first or last row. This is where a list *opens*; a choice made afterwards goes through
    // LazyView, which moves the view as little as it can get away with.
    float ViewFor(int i) const {
        const float most = (std::max)(0.0f, FullHeight() - PanelH()) / rowH;
        return std::clamp((float)i, 0.0f, most);
    }
    // How many rows the panel has room for, as a fraction: the offset is a float and so is this,
    // because a drag of the bar can leave the list half a row down and the arithmetic that keeps
    // a row on screen has to stay right at that offset too.
    float RowsShown() const { return (std::max)(1.0f, (PanelH() - 2 * kPad) / rowH); }
    // The offset to look at a choice from, moving the view as little as will do: the offset it is
    // already at when the chosen row is inside the window with a row to spare on each side, and
    // otherwise the nearest offset that gives it one.
    //
    // The margin is the whole point. Bringing the choice to the control's own row -- what
    // ViewFor does, and what opening wants -- is right for a list that is *being opened*, where
    // the popup covers the control and the two names are meant to be in the same place. It is
    // wrong afterwards: a wheel through a list is a wheel through a list, the highlight moves
    // down it, and a view that jumped to put every choice on the same row threw away wherever
    // the reader had scrolled to and showed them the top of the list again.
    float LazyView(int i) const {
        const float rows = RowsShown();
        const int n = (int)options.size();
        const float most = (std::max)(0.0f, FullHeight() - PanelH()) / rowH;
        // Row i-1 above it and row i+1 below, where those exist: the two constraints on the
        // offset, as a range to keep it inside rather than a value to set it to.
        const float lo = (i < n - 1) ? (float)i + 2.0f - rows : (float)i + 1.0f - rows;
        const float hi = (i > 0) ? (float)i - 1.0f : (float)i;
        const float a = (std::max)(0.0f, lo), b = (std::min)(most, hi);
        if (a > b) return std::clamp((float)i, 0.0f, most);   // a window too short to keep both
        return std::clamp(viewTo, a, b);
    }
    // The panel's own top for a choice: high enough above the control's own row that the chosen
    // row comes out on it, given the offset the list has to be at. For a list that fits, that
    // offset is nothing and this is the chosen row's own height above the control, so the panel
    // steps up the list with the choice -- which is what a popup over a control does, and where
    // the list is *not* windowed. For one too long to show, it is the panel's top edge over the
    // control's row, the same place for every choice the panel can keep to itself.
    float TopFor(int i) const { return RowLine() - kPad - rowH * ((float)i - ViewFor(i)); }
    // The list's own offset, in rows: which row is at the top of the panel's window. It trails
    // `viewTo` -- the chosen row's offset whenever the choice moves, and wherever the bar was
    // dragged to otherwise -- so the list slides to a new choice rather than jumping, and the
    // bar can scroll it away from the choice without choosing anything. Kept inside the range
    // that has a row at both ends of the window: outside it the panel would be showing rows that
    // do not exist, which is blank space. See ViewFor.
    float slid = 0.0f;
    // Where the panel's top edge is to be, in the page's own DIPs: TopFor(selected), so that the
    // chosen row comes out on the control's own row wherever the room allows it. Worked out when
    // the choice moves and left alone in between -- a bar dragged over an open list scrolls the
    // list inside a panel that stays where it is, which is the whole difference between this and
    // a list dragged bodily about the page.
    float place = 0.0f;
    // Where the panel is *drawn*, following `place` at the same lag the list follows its own
    // offset. The rows are drawn from this and not from `place`, which is what makes the panel
    // and the list one thing: a notch of the wheel through a list too short to have a window in
    // it moves the whole popup, and the mark it leaves behind is one the rows travel under -- see
    // drift for the other half of that.
    float lid = 0.0f;
    // What `slid` is travelling to. Two callers and no more: the choice moving, and a drag of
    // the bar.
    float viewTo = 0.0f;
    // How long the slide takes to close most of its gap, in seconds. A follower rather than
    // a curve with a duration, like the page's scroll and the segmented indicator's block:
    // a spun wheel retargets this several times inside one frame, and a storyboard restarted
    // that often would stutter between the notches.
    static constexpr float kSlideLag = 0.05f;
    // How far the mark is drawn off the control's own row, in DIPs: the chosen row's own place,
    // followed rather than stepped. This is the mark's one motion -- the list under it may be
    // scrolled and the panel it is in may be slid, and neither of those is the mark's business --
    // and it is a follower because a choice that changes has to be *seen* to change: the row does
    // not move, so the mark is the only thing on screen that can show it. Where the row itself is
    // what is moving -- a bar being dragged, a wheel with Shift -- the mark is put on the row
    // outright instead; see scrolledByHand.
    float drift = 0.0f;
    // The room's own top edge as it stood when the list opened. A page scrolled under an open list
    // takes the list's place in that page with it -- what the list hangs off has moved -- so the
    // list is closed rather than left pointing at a page position that is no longer there. The
    // room is in the page's own coordinates and only the page moves it, which is what makes its
    // top the thing to watch: a window moved on the screen moves none of it. See Tick.
    float roomTop = 0.0f;
    // Whether the view is being moved by hand rather than to follow a choice. A drag of the bar
    // and a shifted wheel are the same thing to everything that draws: the list is what moves,
    // the mark is on one of its rows, and the two have to travel together -- a mark that read the
    // view's *target* would reach the end of the drag before its row did.
    bool scrolledByHand = false;
    // Its scroll bar, the page's own control: WinUI's drop-down is a ScrollViewer, and
    // the bar in it is the one every other ScrollViewer has. Made the first time a list
    // needs one, not for every drop-down on every layout.
    std::unique_ptr<ScrollBar> bar;
    // The press went down on the bar, so letting go is not choosing a row.
    bool barGrab = false;

    // Typing to find an option. The control is a list of words, so the keyboard can be a
    // search: the letters go into a prefix, the option that starts with it is chosen, and an
    // open list slides to it. Windows' own combo boxes do this, and it needs no field, no
    // layout and nothing drawn -- the chosen option's label is already on the control.
    //
    // The prefix is forgotten after a second of quiet, and whenever the list opens or closes:
    // it is a way of pointing at one option, not a query that stays.
    std::wstring typed;
    ULONGLONG typedAt = 0;
    static constexpr ULONGLONG kTypeWindow = 1000;   // ms

    // What a letter that found nothing does. A control that swallows a key in silence looks
    // broken, and the window's answer to a key nobody took is a beep -- a complaint from the
    // operating system rather than from the thing that did not move. So the mark is the
    // answer: it shrinks for a moment and springs back. Heard, and nowhere to go with it.
    //
    // Full at the instant of the refusal and decaying from there, which is the shape that
    // needs no state beyond the number itself: a curve with a duration would have to be
    // turned round at the end of itself to come back.
    float refuse = 0.0f;
    static constexpr float kRefuseLag = 0.07f;       // seconds

    // And what a gesture that had nowhere to go does -- the wheel, or Up and Down, at the end
    // of the list. A different shape, because it is a different thing to say: not "no", but
    // "that way, and no further". The edge the gesture is going towards twitches quickly and a
    // short way and holds there; the other edge follows it, more slowly and further, so the
    // mark is *shorter* for as long as either of them is out. Then both spring back.
    float knock = 0.0f;        // the fast edge's impulse, 0..1
    float knockLag = 0.0f;     // and the edge that follows it, which goes further
    int knockDir = 0;          // -1 up a list, +1 down it
    bool knockHeld = false;    // still in the rise, which is where the two differ
    static constexpr float kKnockRise = 0.03f;    // seconds for the fast edge to arrive
    // And the time constant for the edge behind it, which is an approach rather than a ramp
    // and so is *fastest* in its first instant: three of these is 95 % of the way, which is
    // the tenth of a second a ramp over the same distance used to take.
    static constexpr float kKnockFollow = 0.03f;
    static constexpr float kKnockLag = 0.12f;     // and for both to spring back
    static constexpr float kKnockTip = 2.0f;      // DIPs the fast edge moves
    static constexpr float kKnockShove = 6.0f;    // and the edge that follows it

    static bool StartsWith(const std::wstring &s, const std::wstring &prefix) {
        if (prefix.size() > s.size()) return false;
        for (size_t i = 0; i < prefix.size(); i++)
            if ((wchar_t)std::towlower(s[i]) != prefix[i]) return false;
        return true;
    }

    bool Focusable() const override { return true; }
    bool TracksPointer() const override { return open; }
    D2D1_RECT_F Head() const { return open ? head : rect; }

    // The room a list may take: the page's visible strip, or on a window whose page does
    // not scroll, the window under its caption -- which is painted last, over everything.
    D2D1_RECT_F Bounds() const {
        if (!owner || !owner->hwnd) return D2D1_RECT_F{ 0, 0, 0, 0 };
        // In this control's own space rather than the window's, which on a scrolling page
        // are not the same thing: the strip the page shows moves with the scroll while the
        // control's rectangle stays where the layout put it. Measuring against the
        // window's rectangle instead made a control halfway down the page think it had the
        // room the top of the page had, and open downward off the bottom of it.
        const D2D1_RECT_F clip = VisibleArea();
        if (clip.bottom > clip.top) return clip;
        // No scrolling area: the window under its caption, in this control's space too,
        // because the page may still be drawn through a transform.
        float dy = 0.0f, op = 1.0f;
        if (scrolls) owner->ContentTransform(&dy, &op);
        return D2D1_RECT_F{ 0, kCaptionH - dy, owner->ClientW(), owner->ClientH() - dy };
    }
    float FullHeight() const { return rowH * (float)options.size() + 2 * kPad; }
    // The panel's height: as tall as the list, and no taller than the room the page shows it
    // through. A longer list is a list that scrolls inside a panel of that height, which is
    // what the bar on it is for -- and it is what "the whole rounded rectangle stays in the
    // room" means. Drawn to the full height instead, the rows that left the room were still
    // built every frame and then thrown away by a clip.
    float PanelH() const {
        const float room = Height(Bounds());
        return room > 0.0f ? (std::min)(FullHeight(), room) : FullHeight();
    }
    // The panel's top edge: `place`, kept inside the room. Worked out here rather than where
    // `place` is set, so that a room that changes under an open list -- the page scrolled, the
    // window resized -- carries the panel with it instead of leaving it hanging out.
    float PanelTop() const {
        const D2D1_RECT_F b = Bounds();
        if (b.bottom - PanelH() <= b.top) return b.top;
        return std::clamp(place, b.top, b.bottom - PanelH());
    }
    // The same edge as drawn rather than as decided: `lid`, kept inside the room. A slide is
    // trimmed at the room's edges on every frame of it, the way the settled panel is, so the
    // rounded rectangle never leaves the room even while it is on its way somewhere.
    float LidTop() const {
        const D2D1_RECT_F b = Bounds();
        if (b.bottom - PanelH() <= b.top) return b.top;
        return std::clamp(lid, b.top, b.bottom - PanelH());
    }

    // The y a row has to be at to be over the control's own row: the two boxes centred on
    // each other, which for a row and a control of the same height is the two boxes on top
    // of each other -- and is why the popup reads as a lid closing over the control.
    float RowLine() const { return (Head().top + Head().bottom) / 2 - rowH / 2; }
    // Where a row is drawn: inside the panel as it is drawn, at the list's own offset into it. A
    // list with no window in it -- one that fits in the room -- moves as a whole with the choice,
    // because its offset is nothing and the panel is what travels; a longer one scrolls under a
    // panel that stays where the room put it. Both come out of the same two quantities.
    float RowTop(int i) const { return LidTop() + kPad + rowH * ((float)i - slid); }

    // The panel: a window of PanelH() onto the list, its top edge as close to the control's
    // own row as the room allows.
    //
    // **This is what a Windows 11 combo box does**, and it is not "open below and flip when
    // short of room": the popup covers the control with the chosen item on it, so the two
    // names are in the same place and the choice reads as a swap rather than as a menu. The
    // rule is one line of ComboBox::GetNonPannablePopupLayout -- the chosen item is laid out
    // at `cbY + cbHeight/2 - itemHeight/2 - margin.Top` -- which is `place` for the chosen
    // row when the panel has the room, and the nearest it can get when it has not. A list
    // taller than the room is the case that made this a window: drawn whole it was a panel
    // running off the page, with the rows outside it built every frame and clipped away.
    D2D1_RECT_F Frame() const {
        const float top = LidTop();
        return { Head().left, top, Head().right, top + PanelH() };
    }
    // The same panel where it has settled. What the pointer can reach is measured against this
    // one: a panel on its way somewhere is no reason for the mouse to be carried along with it.
    D2D1_RECT_F Rest() const {
        const float top = PanelTop();
        return { Head().left, top, Head().right, top + PanelH() };
    }
    // Everything that follows from the chosen row: the area the mouse can reach and the bar.
    // The rows themselves need no telling -- RowTop reads `slid`.
    //
    // Only while the list is open. Closed there is no popup to reach into, and `head` is
    // whatever the last one left behind -- or nothing at all, if there has never been one --
    // so a choice made from the keyboard while closed would have rewritten the control's own
    // rectangle out of a popup that does not exist. SetOpen puts all of this right before the
    // list is seen again.
    void Sync() {
        if (!open) return;
        const D2D1_RECT_F f = Rest();
        rect = { head.left, (std::min)(head.top, f.top),
                 head.right, (std::max)(head.bottom, f.bottom) };
        SyncBar();
    }
    // The popup as it is drawn *now*: it grows out of the control's own row, each edge
    // that has somewhere to go travelling from that row to where it ends up. Up and down
    // both when the list reaches both ways, which is the ordinary case -- the chosen row
    // sits over the control and its neighbours arrive from under it.
    //
    // WinUI does this with SplitOpenThemeAnimation, whose ClosedLength, OpenedLength and
    // OffsetFromCenter are the three numbers ComboBoxTemplateSettings hands it for exactly
    // this purpose.
    D2D1_RECT_F Shown() const {
        const D2D1_RECT_F f = Frame();
        const float k = openT.value;
        const float top = f.top < head.top ? head.top + (f.top - head.top) * k : head.top;
        const float bot = f.bottom > head.bottom
                              ? head.bottom + (f.bottom - head.bottom) * k
                              : head.bottom;
        return { head.left, top, head.right, bot };
    }
    // Taller than the room it is seen through, which is the only thing the bar is for.
    bool Overflows() const { return FullHeight() > Height(Bounds()) + 0.5f; }

    // The chosen row, by the wheel, the keys, a click or the bar. The panel's place and the
    // bar come out of it -- see Sync -- so there is nothing here to keep in step by hand.
    //
    // Returns whether the choice actually moved, which is what tells a gesture that had
    // nowhere to go from one that had: see Knock.
    bool Select(int i) {
        const int n = (int)options.size();
        if (n <= 0) return false;
        i = std::clamp(i, 0, n - 1);
        const bool moved = (i != selected);
        if (moved) {
            selected = i;
            // The list travels to the choice, and the panel goes with it where it has to: the
            // two are one gesture, and this is the one place either is decided.
            viewTo = LazyView(selected);
            place = TopFor(selected);
            scrolledByHand = false;
            Sync();
            if (onChange) onChange(selected);
        }
        if (BarShown()) { bar->Wake(); bar->Poll(); }
        if (owner) owner->Invalidate();
        return moved;
    }
    // One step of the choice, which is what the wheel, Up and Down and the bar's arrows all
    // amount to. Past either end it wraps when `wrapAround` is set and otherwise goes
    // nowhere -- Select clamps, so a step that ran off an end is the option it started from
    // and reports that nothing moved, which is what the callers read as nowhere to go.
    bool Step(int dir) {
        const int n = (int)options.size();
        if (n <= 0) return false;
        const int want = selected + dir;
        if (!wrapAround) return Select(want);
        return Select(((want % n) + n) % n);
    }
    // A gesture with nowhere to go -- the wheel or Up and Down at the end of the list -- is
    // answered by the mark giving way the way it was pressed and coming back. `dir` is +1 for
    // down a list, -1 for up it, which is the direction the gesture was going.
    void Knock(int dir) {
        knock = 0.0f;
        knockLag = 0.0f;
        knockHeld = true;
        knockDir = dir;
    }
    // The bar, which works in DIPs while the list works in rows, and which scrolls the list:
    // choosing is what clicking a row is for, and a bar that chose as well would pick whichever
    // option happened to be where the pointer was let go.
    void ScrollTo(float to, bool /*glide*/) {
        if (!open) return;
        const float most = (std::max)(0.0f, FullHeight() - PanelH()) / rowH;
        viewTo = (std::min)((std::max)(0.0f, to / rowH), most);
        // The view has been taken off the choice by hand: the mark goes with its row until the
        // choice moves again. See scrolledByHand.
        scrolledByHand = true;
    }
    // The bar's geometry: the panel's own, since it is the panel that scrolls. Its track is
    // what the panel can show, and its value is the list's offset -- the one thing about this
    // control that the bar is in charge of.
    void SyncBar() {
        if (!bar) return;
        const D2D1_RECT_F f = Frame();
        bar->rect = { f.right - 2 - ScrollBar::kSize, f.top + 1, f.right - 2, f.bottom - 1 };
        bar->area = f;
        bar->viewport = PanelH();
        bar->extent = FullHeight();
        bar->value = (std::min)((std::max)(0.0f, slid * rowH),
                                (std::max)(0.0f, FullHeight() - PanelH()));
        bar->drawn = bar->value;
    }
    bool BarShown() const { return open && bar && bar->visible; }

    // Which option is at this point. -1 for anything outside the panel as it is drawn right
    // now -- a row the panel has not slid over yet is not there -- and for the bar.
    //
    // Boxed in on all four sides, and it has to be: this is the box the pointer is in when a
    // row is lit, and it is not the window. The sides matter because the popup is narrow --
    // the control's own width -- and the height alone lit a row for a pointer resting beside
    // it, on the card's title a hand's width away. `Bounds` matters for the same reason one
    // step out: the lid reaches over the title bar whenever the chosen row is low enough in
    // the list, and a pointer dragging the window sits exactly there. Rows the page has cut
    // away are not lit either, because they are not drawn -- see Bounds for the strip.
    int RowAt(float x, float y) const {
        const D2D1_RECT_F s = Shown();
        if (x < s.left || x >= s.right) return -1;
        if (y < s.top || y >= s.bottom) return -1;
        if (!Inside(Bounds(), x, y)) return -1;
        if (BarShown() && Inside(bar->rect, x, y)) return -1;
        const int i = (int)std::floor((y - LidTop() - kPad) / rowH + slid);
        return (i >= 0 && i < (int)options.size()) ? i : -1;
    }

    void SetOpen(bool o) {
        barGrab = false;
        // A list being opened or closed is a fresh gesture at the keyboard.
        typed.clear();
        if (o) {
            head = { rect.left, rect.top, rect.right, rect.top + metric::kControlH };
            open = true;
            z = 1;
            // The panel and the list where the chosen row is on the control's own row, each as
            // far as its own limit allows -- and nothing slides on the way in, because this is
            // what they already are: the first frame is also the resting one.
            slid = viewTo = ViewFor(selected);
            place = TopFor(selected);
            // Opening is a lid growing, not a slide: the panel is put where it is to be before
            // the first frame of it. And the mark starts on its row, wherever that turned out.
            lid = place;
            drift = place + kPad + rowH * ((float)selected - slid) - RowLine();
            scrolledByHand = false;
            roomTop = Bounds().top;
            const bool cut = Overflows();
            if (cut) {
                if (!bar) {
                    bar = std::make_unique<ScrollBar>(
                        [this](float to, bool glide) { ScrollTo(to, glide); });
                }
                bar->owner = owner;
                bar->visible = true;
            }
            // The whole of it takes the mouse, or a click on an option falls through to
            // whatever is behind the list.
            Sync();
            if (cut) {
                // The line shows at once: the pointer is on the control, not over the
                // list, so nothing else would wake it.
                bar->Wake();
                bar->Poll();
            }
        } else {
            open = false;
            // Stays raised while the lid closes, and Tick drops it back to 0 when that is
            // done. Nothing is stolen by that: the rect has gone back to the control's own
            // row, so the hit test cannot reach the rows even though they are still being
            // drawn.
            z = 1;
            if (head.right > head.left) rect = head;
            // Its timers go with it: the bar's own, which stop with the bar. See Timer.
            if (bar) {
                bar->OnRelease();
                bar->hover = false;
                bar->visible = false;
                bar->Poll();
            }
        }
    }
    void Dismiss() override { if (open) SetOpen(false); }
    bool Animating() const override {
        return Widget::Animating() || openT.Wants(open ? 1.0f : 0.0f) ||
               (open && slid != viewTo) ||
               (open && lid != place) ||
               (open && drift != place + kPad + rowH * ((float)selected -
                                                        (scrolledByHand ? slid : viewTo)) -
                                     RowLine()) ||
               refuse > 0.0f || knockHeld ||
               knock > 0.0f || knockLag > 0.0f ||
               (BarShown() && bar->Animating());
    }
    void Tick(float dt) override {
        Widget::Tick(dt);
        openT.To(open ? 1.0f : 0.0f);
        if (open) {
            openT.Step(dt, motion::kFast, motion::Decel);
            // The page has scrolled out from under the list: the list goes, with the same close it
            // gives a click outside itself. Taken before anything else this frame, because there
            // is no point sliding a list that is on its way out, and read here rather than in
            // Paint, where this control writes no state. The room's top is also what a *resize*
            // from the top edge would move; the list would be rebuilt by the layout after that
            // anyway.
            const float roomNow = Bounds().top;
            if (roomNow != roomTop) {
                roomTop = roomNow;      // so that the close's own frames are not asked again
                SetOpen(false);
                return;
            }
            // The list slides while the popup is open, and only then. A click on an option
            // closes the popup on the release, and a panel that then slid its way to the
            // option it had just chosen would be moving the list *while the lid closed over
            // it* -- two motions where there is room for one, and the slide is the one that
            // loses, because it cannot finish. Closed, the panel is left where it was;
            // SetOpen puts it on the chosen row before it is seen again.
            const float want = viewTo;
            if (slid != want) {
                slid += (want - slid) * (1.0f - std::exp(-dt / kSlideLag));
                if (std::fabs(want - slid) < 0.004f) slid = want;
                SyncBar();
            }
            // The panel's own place, followed. This is the whole of the motion a wheel through a
            // list that fits makes: the popup slides a row, the rows go with it, and the mark
            // stays where it was -- options travelling under it rather than it down them.
            if (lid != place) {
                lid += (place - lid) * (1.0f - std::exp(-dt / kSlideLag));
                if (std::fabs(place - lid) < 0.004f) lid = place;
                SyncBar();
            }
            // The mark's own place, followed: where the chosen row is against the panel as the
            // panel *asked* to be -- `place`, not where the room has put it. This is the only
            // motion the mark has that is worth animating: a choice that changes has to be *seen*
            // to change, and the row does not move, so the mark is the only thing that can show
            // it. A view scrolled by hand is put on the row outright instead, because there the
            // row *is* what is moving and a follower is the mark coming loose from the option it
            // points at. And the room's own displacement is not in here at all: it is a move of
            // the room rather than a motion of this control, and PaintMark adds it on at once --
            // a mark that followed it slid with the page for a tenth of a second and then sprang
            // back to where its row was.
            const float driftWant = place + kPad +
                                    rowH * ((float)selected - (scrolledByHand ? slid : viewTo)) -
                                    RowLine();
            if (scrolledByHand) {
                drift = driftWant;
            } else if (drift != driftWant) {
                drift += (driftWant - drift) * (1.0f - std::exp(-dt / kSlideLag));
                if (std::fabs(driftWant - drift) < 0.004f) drift = driftWant;
            }
        } else if (!openT.Step(dt, motion::kFast, motion::Accel)) {
            z = 0;          // the lid has finished closing; stop keeping it raised
        }
        // The refusal, if there is one: an impulse that fades, so a letter with nowhere to go
        // is answered for about a fifth of a second and then is not.
        if (refuse > 0.0f) {
            refuse *= std::exp(-dt / kRefuseLag);
            if (refuse < 0.002f) refuse = 0.0f;
        }
        // And the knock at the end of the list: the fast edge out in a thirtieth of a second
        // and held, the one behind it following in three times that, and both springing back
        // in about a fifth. A lean and a spring rather than a move.
        if (knockHeld) {
            // Both edges set off on this frame, and everything the gesture shows comes of
            // that. The edge behind goes three times as far, so a ramp for it -- three times
            // the distance in three times the time -- would run at exactly the fast edge's
            // rate, and the two of them would move as one for the first thirtieth of a second
            // while the mark slid bodily down and did not shorten at all; which is what this
            // was, and what made it read as the far edge starting late. An approach is fastest
            // in its first instant, so the mark begins losing length at once, and the fast
            // edge still arrives first because its distance is the short one.
            knock = (std::min)(knock + dt / kKnockRise, 1.0f);
            knockLag = 1.0f - (1.0f - knockLag) * std::exp(-dt / kKnockFollow);
            if (knock >= 1.0f && knockLag >= 0.95f) knockHeld = false;
        } else if (knock > 0.0f || knockLag > 0.0f) {
            knock *= std::exp(-dt / kKnockLag);
            knockLag *= std::exp(-dt / kKnockLag);
            if (knock < 0.002f && knockLag < 0.002f) { knock = knockLag = 0.0f; knockDir = 0; }
        }
        if (BarShown()) {
            SyncBar();
            // The bar is not in the window's list, so nothing else tells it the pointer
            // has left: this control's own hover going is what runs this frame.
            if (!barGrab) {
                const D2D1_POINT_2F at = Cursor();
                bar->hover = hover && Inside(bar->rect, at.x, at.y);
            }
            bar->Tick(dt);
        }
    }

    void OnClick() override { Pick(true); }
    void OnActivate() override { Pick(false); }
    // One path for both, because everything but the choice itself is the same: opening the
    // list, the bar's press, and the close. `byPointer` is the whole difference -- a click is
    // the pointer naming a row, and Space or Enter is naming nothing, which is the row the
    // accent mark has been left on. Reading the pointer for those chose whichever row happened
    // to be under a mouse that was resting somewhere else on the screen entirely.
    void Pick(bool byPointer) {
        if (!enabled) return;
        // A press on the list's bar scrolls it. It does not choose, and it does not close.
        if (barGrab) { barGrab = false; return; }
        if (!open) { SetOpen(true); return; }
        // Through the same function the drawing uses, so the row that was lit and the row
        // that is chosen cannot come apart.
        const D2D1_POINT_2F at = Cursor();
        const int i = byPointer ? RowAt(at.x, at.y) : selected;
        if (i >= 0) Select(i);
        SetOpen(false);
    }
    bool OnKey(WPARAM vk) override {
        // A bar drag that ended outside the control never reached OnClick; the key that
        // follows is not its release.
        barGrab = false;
        if (vk == VK_ESCAPE && open) { SetOpen(false); return true; }
        if (vk != VK_UP && vk != VK_DOWN) return false;
        const int n = (int)options.size();
        if (n <= 0) return false;
        // Open, a step goes through Select like the wheel's does, so the rows slide under
        // the control as the choice moves -- and a step that runs off an end is a gesture with
        // nowhere to go, which is the knock's. Closed, the label is the only thing that moves
        // and there is no mark on screen to give way, so the ends are silent.
        if (open) {
            const int dir = vk == VK_DOWN ? 1 : -1;
            if (!Step(dir) && !wrapAround) Knock(dir);
            return true;
        }
        Step(vk == VK_DOWN ? 1 : -1);
        return true;
    }
    // A printable character, from the keyboard or the IME. Always the control's, whether or
    // not it found anything: a letter that matched nothing and was passed on to the window
    // would be answered with a beep.
    //
    // **Only while the list is open.** A closed drop-down is a button with a label on it: it
    // has nothing to search in, and the mark that would show what the search found is not on
    // screen. Space opens it, and the search is there.
    bool OnChar(wchar_t ch) override {
        const int n = (int)options.size();
        if (!enabled || n <= 0) return false;
        if (!open) return true;      // consumed, so the window does not beep at it
        const ULONGLONG now = GetTickCount64();
        const wchar_t lower = (wchar_t)std::towlower(ch);
        // The same letter again steps to the next option that starts with it rather than
        // looking for the prefix it is already on. That is what Windows does, and it is the
        // only way to reach the second "Monthly" from the keyboard.
        const bool again = typed.size() == 1 && typed[0] == lower;
        if (again || now - typedAt > kTypeWindow) typed.clear();
        typedAt = now;
        typed.push_back(lower);
        // Where the search starts, and this is the whole of it: a first letter is a step, like a
        // notch of the wheel, and a step goes forward -- the next option that starts with it,
        // after the one chosen now. Every letter after it only refines an answer that has
        // already been given, so it starts *at* the chosen option, and the answer stays put for
        // as long as the option under it still starts with what has been typed. It moves on only
        // when that option has been typed out of the running.
        //
        // Starting after the chosen option every time is what this was, and it made every letter
        // a fresh errand: with five "Every ..." options in a row, E V E R Y walked through all
        // five of them, a letter each, so the name that was typed out was never the name that
        // ended up chosen. A search that walks a list while a name is typed into it is not what
        // Explorer does, and Explorer is the behaviour people arrive with.
        const int from = typed.size() > 1 ? selected : selected + 1;
        for (int step = 0; step < n; step++) {
            const int i = (from + step) % n;
            if (StartsWith(options[i], typed)) { Select(i); return true; }
        }
        // Nothing starts with it. Answered rather than ignored: see `refuse`.
        refuse = 1.0f;
        if (owner) owner->Invalidate();
        return true;
    }

    // The list's bar, driven from here: it is not one of the window's controls, because
    // the window's list is rebuilt on every layout and this bar lives as long as the list.
    // No offset to take off anywhere: the popup is drawn where it was placed, and it is
    // the rows that move inside it rather than it moving over them.
    void OnPress(float x, float y) override {
        barGrab = false;
        if (!BarShown()) return;
        SyncBar();
        if (!Inside(bar->rect, x, y)) return;
        barGrab = true;
        bar->hover = true;
        bar->OnPress(x, y);
    }
    void OnDrag(float x, float y) override {
        if (barGrab && BarShown()) bar->OnDrag(x, y);
    }
    void OnRelease() override {
        if (barGrab && BarShown()) bar->OnRelease();
    }
    void OnPointerMove(float x, float y) override {
        if (!BarShown()) return;
        SyncBar();
        if (!barGrab) bar->hover = hover && Inside(bar->rect, x, y);
        bar->OnPointerMove(x, y);
    }
    bool OnWheel(float x, float y, float notches) override {
        if (!open) return false;
        // Where the wheel lands does not matter while the list is up. Inside the list it is the
        // list's, as below. Outside it the turn is still taken and nothing is done with it --
        // which is what a combo box does: the page under an open list does not move at all, so
        // there is nothing for the list to be dragged out of, and the list itself stays put. A
        // row of options is not a page: a wheel that scrolled the page out from under the list,
        // or scrolled the list past a choice that is not moving, is worse than one that lands on
        // nothing.
        if (!Inside(Shown(), x, y)) return true;
        // Shift asks for the list to be *scrolled* rather than chosen from: the same wheel
        // over the same list, aimed at the panel instead of at the choice. A list that fits
        // has nothing to scroll, and there the gesture does nothing at all -- which is what
        // Shift over a control with nothing to scroll means, and is better than a wheel that
        // quietly takes an option because Shift was not understood.
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            if (Overflows()) {
                int lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                if (lines == WHEEL_PAGESCROLL) lines = 6;
                // Rows, not DIPs: this list is counted in rows, and the system's line count
                // read as a row count is the same gesture the page above it uses.
                ScrollTo(viewTo * rowH - notches * (float)lines * rowH, false);
                if (BarShown()) { bar->Wake(); bar->Poll(); }
            }
            return true;
        }
        // One row a notch, and the row that arrives at the control's own row is the choice.
        // A notch is a step through the items here and not a distance: this is a list of
        // things to choose, and a wheel that moved it 66 DIPs, as the page's does, would
        // leave the chosen row somewhere other than under the control it was chosen from.
        //
        // Taken even at either end. Passed on, it would scroll the page out from under an
        // open list -- and on a page that lays itself out in response to a scroll, take the
        // list away with it.
        const int step = (int)std::lround(-notches);
        const int dir = step != 0 ? step : (notches > 0.0f ? -1 : 1);
        // A ring has no end to run into, so there is nothing for the mark to give way to:
        // the step either moves the choice or has come all the way round to where it was.
        if (!Step(dir) && !wrapAround) Knock(dir > 0 ? 1 : -1);
        if (BarShown()) { bar->Wake(); bar->Poll(); }
        return true;
    }

    // The mark on the chosen row: the accent bar, which shrinks for a letter that found
    // nothing and gives way for a gesture that had nowhere to go. Drawn by the popup, which is
    // the only place it is: a closed drop-down has no mark, and nothing to search in either.
    //
    // It goes on the chosen row, at that row's own place -- and which of the two motions brings
    // it there is the whole of what this control feels like. A wheel through a list that fits the
    // room slides the *panel*: the popup moves a row, the rows go with it, and the mark stands
    // still on the control's own row while a different option comes under it, which is a dial
    // rather than a menu. A wheel through a longer list moves the *choice* down a window that
    // stays where it is, and there the mark is what travels -- by one row per notch, followed, so
    // that the change is seen. What it never does is read an animating offset as if it were a
    // settled one: see scrolledByHand, which is the one case where the row is the moving thing
    // and the mark is carried along with it exactly.
    void PaintMark(const Painter &p, float alpha) {
        const D2D1_RECT_F h = Head();
        // The followed half and the room's own half, added at once. The clamp is what the room
        // did to the panel, not a motion of the control's: the rows are drawn from it too, so it
        // has to be where they are on this frame rather than where a follower has got to.
        const float line = RowLine() + drift + (PanelTop() - place);
        const float inset = 8.0f + 4.8f * refuse;    // the refusal's own shrink
        // The knock, in DIPs of offset per edge. The fast edge is capped at what the edge
        // behind it has already given: the mark may be shorter than it is at rest and never
        // longer, so what the two of them do together reads as length rather than as travel.
        const float tip = (std::min)(kKnockTip * knock, kKnockShove * knockLag);
        const float shove = kKnockShove * knockLag;
        const float top = line + inset + (knockDir > 0 ? shove : -tip);
        const float bot = line + rowH - inset + (knockDir > 0 ? tip : -shove);
        p.rt->FillRoundedRectangle(
            D2D1::RoundedRect({ h.left + 1, top, h.left + 4, bot }, 1.5f, 1.5f),
            p.Brush(Fade(p.pal->accent, alpha)));
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        const D2D1_RECT_F h = Head();
        // Everything this control draws is inside the room the page gives it, the control's own
        // box included: that box is part of the page, and when the page is scrolled it goes under
        // the header with the rest of the page rather than over it. The page no longer clips a
        // pass that floats over it -- see the note in Window::Paint -- so the control does it
        // itself, which is also the answer to "what is the room": the same `Bounds()` the clamp
        // keeps the panel inside. It is the shadow's own room too, and the only place that shows:
        // everything else in it is drawn inside the room by construction.
        const D2D1_RECT_F room = Bounds();
        const bool roomy = room.right > room.left && room.bottom > room.top;
        if (roomy) p.rt->PushAxisAlignedClip(room, D2D1_ANTIALIAS_MODE_ALIASED);
        p.FillRound(h, metric::kRadiusControl,
                    !enabled ? c.controlBg
                             : Mix(c.controlBg, c.controlBgHover,
                                   open ? 1.0f : hoverT));
        p.StrokeRound(h, metric::kRadiusControl, c.controlStroke);
        const D2D1_COLOR_F fg = enabled ? c.textPrimary : c.textDisabled;
        if (selected >= 0 && selected < (int)options.size())
            p.Text(options[selected], { h.left + 11, h.top, h.right - 32, h.bottom },
                   p.font->body, fg);
        p.Text(glyph::kChevronDown, { h.right - 28, h.top, h.right, h.bottom }, p.font->icon,
               c.textSecondary);
        if (focus && owner && owner->showFocusRing) {
            const D2D1_RECT_F o = { h.left - 2, h.top - 2, h.right + 2, h.bottom + 2 };
            p.StrokeRound(o, metric::kRadiusControl + 2, c.textPrimary, 2.0f);
        }
        const float openF = openT.value;
        // Both of these leave with the room's clip popped. A clip that is pushed and not popped
        // stays on for the rest of the frame -- the window draws the rest of the page into the
        // same target -- and a clip stack that does not balance is a frame Direct2D throws away:
        // the page came up empty, with nothing but the backdrop, while the app went on working.
        if (openF <= 0.0f) {
            if (roomy) p.rt->PopAxisAlignedClip();
            return;
        }

        // Where the lid is *now*, and the rows where they rest. The two are separate, and
        // that separation is the whole of the animation: the frame grows out of the
        // control's own row while the rows stay exactly where the layout put them, so the
        // chosen row is over the control from the first frame to the last, and what opens
        // is a window onto a list rather than a list that slides into place.
        const D2D1_RECT_F s = Shown();
        if (s.bottom - s.top < 2.0f) {
            if (roomy) p.rt->PopAxisAlignedClip();
            return;
        }

        // A flyout is not a card: it is over the page rather than part of it, so it gets
        // an opaque surface of its own and a shadow.
        //
        // The shadow is Fluent's, which is a blur, and there is no cheap real blur in
        // Direct2D without an effect and a layer per frame -- so this is twelve rounded
        // rectangles at 0.9 per cent, the largest and faintest outermost, which stack into
        // a falloff smooth enough to read as one. It was five at four per cent, and five at
        // four per cent is a black band with an edge on it: at the size of a flyout the
        // steps show as bands, and the four per cent at the outermost layer is not faint
        // enough to disappear. Four DIPs down and none up, because the light is above the
        // popup and the popup hangs off the control it belongs to.
        constexpr int kShadowLayers = 12;
        constexpr float kShadowReach = 14.0f;
        constexpr float kShadowDrop = 4.0f;
        for (int i = kShadowLayers; i >= 1; i--) {
            const float e = kShadowReach * (float)i / (float)kShadowLayers;
            p.FillRound({ s.left - e, s.top - e + kShadowDrop,
                          s.right + e, s.bottom + e + kShadowDrop },
                        8.0f + e, Fade(Rgb(0x000000, 0.009f), openF));
        }
        p.FillRound(s, 8.0f, Fade(c.flyoutBg, openF));
        p.StrokeRound(s, 8.0f, Fade(c.flyoutStroke, openF));

        // The rows, clipped to the lid: what it has not grown over yet is not drawn, and a
        // row the leading edge has reached half way through is cut there.
        p.rt->PushAxisAlignedClip({ s.left, s.top + 1, s.right, s.bottom - 1 },
                                  D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_POINT_2F at = open ? Cursor() : D2D1::Point2F();
        const int hot = open ? RowAt(at.x, at.y) : -1;
        for (size_t i = 0; i < options.size(); i++) {
            const float top = RowTop((int)i);
            const D2D1_RECT_F row = { s.left + 4, top, s.right - 4, top + rowH };
            if (row.bottom <= s.top || row.top >= s.bottom) continue;
            const bool over = (int)i == hot;
            // The row under the pointer is filled, and filled harder while the button is
            // down: the press has to land somewhere, and the release takes the list away.
            if (over)
                p.FillRound(row, metric::kRadiusControl,
                            Fade(pressed && enabled ? c.controlBgPressed : c.subtleHover, openF));
            p.Text(options[i], { row.left + 7, row.top, row.right, row.bottom },
                   p.font->body, Fade(c.textPrimary, openF));
        }
        // The mark goes on the chosen row, and it is the row's own place that is read here -- not
        // the list's offset, which would carry the mark along with the list. The two agree at
        // rest and part company for the tenth of a second a slide takes, which is the slide the
        // mark is supposed to be standing still through. Drawn inside the rows' clip, so a row
        // scrolled out of the window takes the mark with it.
        PaintMark(p, openF);
        // The bar once the list has arrived: its line is a setter, and at full strength
        // over a list still growing it would arrive first.
        if (BarShown() && openF >= 1.0f) {
            SyncBar();
            bar->Paint(p);
        }
        p.rt->PopAxisAlignedClip();
        if (roomy) p.rt->PopAxisAlignedClip();
    }
};

}  // namespace micula
