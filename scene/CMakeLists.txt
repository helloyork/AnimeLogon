add_library(animelogon_scene_host STATIC src/host.cpp)
target_include_directories(animelogon_scene_host PUBLIC include PRIVATE src)
target_link_libraries(animelogon_scene_host PUBLIC animelogon_common d3d11 dxgi dxguid userenv)
add_library(animelogon_scene_preview STATIC src/preview.cpp ${PROJECT_SOURCE_DIR}/overlay/src/player.cpp)
target_include_directories(animelogon_scene_preview PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
target_link_libraries(animelogon_scene_preview PUBLIC animelogon_scene_host mf mfplat mfreadwrite mfuuid)
include(${PROJECT_SOURCE_DIR}/cmake/CompileShader.cmake)
animelogon_compile_shader(animelogon_scene_preview ${PROJECT_SOURCE_DIR}/overlay/shaders/video.hlsl VS vs_5_0 g_preview_vs preview_video_vs.h)
animelogon_compile_shader(animelogon_scene_preview ${PROJECT_SOURCE_DIR}/overlay/shaders/video.hlsl PS ps_5_0 g_preview_ps preview_video_ps.h)
animelogon_add_executable(scene SOURCES src/main.cpp src/renderer.cpp src/lottie.cpp DESCRIPTION "AnimeLogon 场景渲染器"
    LIBS d3d11 dxgi dxguid d3dcompiler winmm thorvg)
configure_file(src/licenses.rc.in ${CMAKE_CURRENT_BINARY_DIR}/licenses.rc @ONLY)
target_sources(scene PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/licenses.rc)
target_include_directories(scene PRIVATE include src
    ${PROJECT_SOURCE_DIR}/vendor/thorvg/src/common
    ${PROJECT_SOURCE_DIR}/vendor/thorvg/src/renderer
    ${PROJECT_BINARY_DIR}/vendor/thorvg/config)
