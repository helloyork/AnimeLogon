# DirectXTex (may2026), only what compresses pictures to BC7 on the graphics card and to BC4 on the
# processor. The compute shaders are compiled ahead of time into Shaders/Compiled.
set(S ${CMAKE_CURRENT_SOURCE_DIR}/DirectXTex)
add_library(directxtex STATIC
    ${S}/BC.cpp
    ${S}/BC4BC5.cpp
    ${S}/BC6HBC7.cpp
    ${S}/BCDirectCompute.cpp
    ${S}/DirectXTexCompress.cpp
    ${S}/DirectXTexCompressGPU.cpp
    ${S}/DirectXTexConvert.cpp
    ${S}/DirectXTexImage.cpp
    ${S}/DirectXTexMipmaps.cpp
    ${S}/DirectXTexUtil.cpp
)
target_include_directories(directxtex PUBLIC ${S} PRIVATE ${S}/Shaders/Compiled)
target_link_libraries(directxtex PUBLIC d3d11 dxgi windowscodecs ole32)
target_compile_options(directxtex PRIVATE /W3 /wd4100 /wd4189 /wd4244 /wd4267 /wd4456 /wd4458 /wd4996)
