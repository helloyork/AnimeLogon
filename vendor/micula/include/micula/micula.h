// Micula / micula.h
//
// The one header to include. Micula is header-only: theme.h holds the palette, the
// motion curves and the fonts; window.h the composed window, the painter and the Widget
// base; widgets.h the controls. Each includes the one before it, so this is widgets.h
// under a name that will not change if the files are ever split differently.
//
// What the program has to provide, and nothing else:
//
//   - link    d2d1 d3d11 dxgi dcomp dwrite dwmapi imm32 windowscodecs user32 gdi32
//             ole32 advapi32. window.h names every one with #pragma comment, so MSVC
//             needs none of them on its command line; other toolchains do.
//   - COM     initialised on the UI thread (CoInitializeEx, apartment-threaded) before
//             Window::Create. The caption icon and Window::Image go through WIC, which
//             is COM; without it both are silently absent.
//   - DPI     per-monitor v2, ideally from the program's manifest. EnablePerMonitorDpi()
//             sets it from code as well, for launches that bring their own activation
//             context.

#pragma once

#define MICULA_VERSION_MAJOR 0
#define MICULA_VERSION_MINOR 1
#define MICULA_VERSION_PATCH 0
#define MICULA_VERSION_STRING "0.1.0"

#include "widgets.h"
