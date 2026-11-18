# DirectXTex may2026

Source: https://github.com/microsoft/DirectXTex/releases/tag/may2026
License: MIT; see LICENSE.

Only the files that compress a picture are kept: BC7 on the graphics card through DirectCompute
(or on WARP when there is no usable card) and BC4 on the processor. Everything that reads or
writes image files (WIC, DDS, TGA, HDR) and the processor's own BC7 encoder path are not built.

`DirectXTex/Shaders/Compiled/*.inc` were produced from `BC6HEncode.hlsl` and `BC7Encode.hlsl`
with the Windows SDK's fxc, the same entry points and options as `CompileShaders.cmd`
(`/Tcs_5_0`, and `/Tcs_4_0 /DEMULATE_F16C` for the `_cs40` variants), without debug data.
No source file is changed.

Only the sandboxed scene renderer links it, when it imports a scene.
