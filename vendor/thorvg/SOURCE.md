# ThorVG 1.1.2

Source: https://github.com/thorvg/thorvg/releases/tag/v1.1.2
License: MIT; see LICENSE and the notices in each source file.

Only the CPU renderer, Lottie loader and embedded PNG/JPEG loaders are built.
Expressions, fonts, media, disk IO and GPU backends are disabled. No JavaScript
engine is included. x64 does not require AVX; ARM64 uses its standard NEON.

Local change: the four global new/delete replacements at the end of
src/renderer/tvgInitializer.cpp were removed, retaining the C++ runtime's
allocation and failure semantics. The upstream source list is explicit in CMake.
