// Micula / widgets.h
//
// The control vocabulary: Button, CheckBox, ToggleSwitch, Segmented, Slider,
// ScrollBar, DropDown, SideNav, TextBox and ProgressBar.
//
// Kept small on purpose. Every control here is drawn by hand in four states, and the
// next one is not free -- it is another rest/hover/pressed/disabled quartet to get
// right in both themes, another Tab stop to wire, and another thing that can look
// subtly unlike Windows. When a page needs something this list does not have, the
// first answer is to express it with what is here; the second is a Widget subclass
// of the page's own, which is exactly what these are.
//
// SideNav is here for a different reason: it is not a control a page repeats, it is a
// piece of a *window*. Every window that has one would otherwise build it again out of
// its own constants -- which is what examples/settings did, forty lines a window of
// rows, a travelling accent bar, a hit test and a layout of its own.
//
// Every geometry number below is in DIPs and comes from Microsoft's own control
// specs (a 32-DIP control height, a 20-DIP checkbox, a 40x20 switch with a 12-DIP
// knob). They are written as literals rather than named constants where they are
// used once, because a constant named kKnobInset that appears in one expression is
// harder to check against the spec than the number itself.
//
// Each control has a header of its own -- button.h, text_box.h and so on -- and this file
// is the set of them: include it and you have every one, or include the single control a
// page draws and parse nothing else.

#pragma once

#include "window.h"

#include "button.h"
#include "check_box.h"
#include "drop_down.h"
#include "progress_bar.h"
#include "scroll_bar.h"
#include "segmented.h"
#include "side_nav.h"
#include "slider.h"
#include "text_box.h"
#include "toggle_switch.h"
