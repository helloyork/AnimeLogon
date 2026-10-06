// Registers keyboard and mouse raw input so presses reach this window even though it never
// holds focus, and drains them into a wake::Intent. See wake.h for what is and is not kept.
#pragma once

#include <windows.h>

#include "wake.h"

namespace inputsink {

inline bool Register(HWND window) {
    RAWINPUTDEVICE devices[] = {
        {0x01, 0x02, RIDEV_INPUTSINK, window},  // mouse
        {0x01, 0x06, RIDEV_INPUTSINK, window},  // keyboard
    };
    return RegisterRawInputDevices(devices, ARRAYSIZE(devices), sizeof(devices[0])) != FALSE;
}

inline void Unregister() {
    RAWINPUTDEVICE devices[] = {
        {0x01, 0x02, RIDEV_REMOVE, nullptr},
        {0x01, 0x06, RIDEV_REMOVE, nullptr},
    };
    RegisterRawInputDevices(devices, ARRAYSIZE(devices), sizeof(devices[0]));
}

// One WM_INPUT message. Returns true if it was a fresh press.
inline bool ReadMessage(HRAWINPUT handle, wake::Intent &intent, DWORD messageTick) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    const UINT copied = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (copied == (UINT)-1 || copied < sizeof(RAWINPUTHEADER) || input.header.dwSize > copied) return false;
    return intent.ObserveRaw(input, messageTick);
}

// Drain the queue before a decision depends on it. Returns true if any was a fresh press.
inline bool Drain(HWND window, wake::Intent &intent) {
    bool pressed = false;
    MSG msg{};
    while (PeekMessageW(&msg, window, WM_INPUT, WM_INPUT, PM_REMOVE)) {
        pressed = ReadMessage(reinterpret_cast<HRAWINPUT>(msg.lParam), intent, msg.time) || pressed;
        DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
    }
    return pressed;
}

}  // namespace inputsink
