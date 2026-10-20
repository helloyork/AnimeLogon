// Deciding that somebody has arrived, from key and button presses only.
//
// Privacy: only the down edge of a key or button is looked at, and every record of which
// keys are held is erased the moment presence is established. Nothing is logged by key and
// nothing is forwarded. The one key that wakes the screen is withheld from the password box
// behind the video; every other key reaches Windows untouched.
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>

namespace wake {

using Keys = std::array<bool, 256>;

// Fallback for when raw input cannot be registered.
inline Keys CaptureKeys() {
    Keys keys{};
    for (int key = 1; key < 255; ++key) keys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    return keys;
}

// Raw input presence tracker. Keys already held when the screen came up (Win+L), events
// queued before it, and auto-repeat do not count.
class Intent {
public:
    void Prime(const Keys &held, DWORD activationTick) {
        requested_ = false;
        held_ = held;
        polled_ = held;
        armedAt_ = activationTick;
        armed_ = true;
    }

    void Suspend() {
        requested_ = false;
        held_ = {};
        polled_ = {};
        armed_ = false;
    }

    bool active() const { return armed_ && !requested_; }
    bool requested() const { return requested_; }

    // True when this packet is a fresh press.
    bool ObserveRaw(const RAWINPUT &input, DWORD messageTick) {
        if (!active() || static_cast<LONG>(messageTick - armedAt_) <= 0) return false;
        if (input.header.dwType == RIM_TYPEKEYBOARD) {
            if (input.header.dwSize < offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)) return false;
            const RAWKEYBOARD &kb = input.data.keyboard;
            if (kb.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return false;
            UINT key = kb.VKey;
            if (key == 0 || key == VK_PACKET || key >= 255) key = 0;
            // Side-specific modifiers, so releasing one cannot make the other's repeat
            // look like a new press.
            if (key == VK_SHIFT && kb.MakeCode != 0) key = kb.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
            else if (key == VK_CONTROL) key = (kb.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
            else if (key == VK_MENU) key = (kb.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
            if (kb.Flags & RI_KEY_BREAK) {
                held_[key] = false;
                return false;
            }
            const bool wasHeld = held_[key];
            held_[key] = true;
            return !wasHeld && Request();
        }
        if (input.header.dwType == RIM_TYPEMOUSE) {
            if (input.header.dwSize < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) return false;
            const USHORT flags = input.data.mouse.usButtonFlags;
            static const USHORT down[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
                                          RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN,
                                          RI_MOUSE_BUTTON_5_DOWN};
            static const USHORT up[] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
                                        RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
            static const UINT vk[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            bool pressed = false;
            for (size_t i = 0; i < ARRAYSIZE(vk); ++i) {
                if (flags & down[i]) {
                    pressed = !held_[vk[i]] || pressed;
                    held_[vk[i]] = true;
                }
                if (flags & up[i]) held_[vk[i]] = false;
            }
            return pressed && Request();
        }
        return false;
    }

    bool ObserveKeys(const Keys &keys) {
        if (!active()) return false;
        bool pressed = false;
        for (size_t key = 1; key < 255; ++key) {
            if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
            pressed = (keys[key] && !polled_[key]) || pressed;
        }
        polled_ = keys;
        return pressed && Request();
    }

private:
    bool Request() {
        requested_ = true;
        held_ = {};
        polled_ = {};
        return true;
    }

    Keys held_{};
    Keys polled_{};
    DWORD armedAt_ = 0;
    bool armed_ = false;
    bool requested_ = false;
};

// A key that would type into, or submit, the password box. Chords, modifiers, locks,
// function keys, Escape and Tab are left to Windows.
inline bool IsWakeKey(DWORD vk, bool ctrl, bool alt, bool win) {
    if (ctrl || alt || win) return false;
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case VK_APPS: case VK_ESCAPE: case VK_TAB:
        return false;
    default:
        break;
    }
    return !(vk >= VK_F1 && vk <= VK_F24);
}

// The per-screen latch the keyboard hook keeps: one key per screen is withheld.
struct KeyLatch {
    bool taken = false;       // a key has been taken on this screen
    DWORD heldVk = 0;         // ...and is still down, so its repeats and release go too
    bool litDisplay = false;  // a key has lit the panel since it last went dark
};

struct KeyState {
    bool armed = false;       // the video is up and nobody has woken it yet
    bool displayOff = false;
};

enum class KeyVerdict {
    Pass,         // give it to Windows
    Withhold,     // swallow, nothing else
    WithholdWake, // swallow, and it wakes the screen
};

// Updates `latch`. A key pressed at a dark panel lights the panel and does not wake.
inline KeyVerdict DecideKey(KeyLatch &latch, const KeyState &s, DWORD vk, bool down, bool up, bool isWakeKey) {
    if (latch.heldVk != 0 && vk == latch.heldVk) {
        if (up) latch.heldVk = 0;
        return KeyVerdict::Withhold;
    }
    if (!down || !isWakeKey || !s.armed) return KeyVerdict::Pass;
    // Withheld so it cannot land in the box behind the off panel.
    if (s.displayOff && !latch.litDisplay) {
        latch.heldVk = vk;
        latch.litDisplay = true;
        return KeyVerdict::Withhold;
    }
    if (latch.taken) return KeyVerdict::Pass;
    latch.heldVk = vk;
    latch.taken = true;
    return KeyVerdict::WithholdWake;
}

}  // namespace wake
