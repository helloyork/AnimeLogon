# Console programs, started with CREATE_NO_WINDOW on the Winlogon desktop.
include(${PROJECT_SOURCE_DIR}/cmake/CompileShader.cmake)

animelogon_add_executable(overlay
    SOURCES
        src/overlay.cpp
        src/presenter.cpp
        src/player.cpp
        src/audio.cpp
        src/screen.cpp
        src/keyhook.cpp
        src/plan.cpp
        src/bake.cpp
        src/clockface.cpp
        src/clocktext.cpp
    DESCRIPTION "AnimeLogon 登录界面视频"
    LIBS d3d11 dxgi dcomp dwmapi d2d1 dwrite dxguid mf mfplat mfreadwrite mfuuid windowscodecs xaudio2 mmdevapi
         wtsapi32 shcore runtimeobject)

animelogon_compile_shader(overlay ${CMAKE_CURRENT_SOURCE_DIR}/shaders/video.hlsl
    VS vs_5_0 g_vs video_vs.h)
animelogon_compile_shader(overlay ${CMAKE_CURRENT_SOURCE_DIR}/shaders/video.hlsl
    PS ps_5_0 g_ps video_ps.h)

animelogon_add_executable(launcher
    SOURCES src/launcher.cpp
    DESCRIPTION "AnimeLogon 登录界面视频服务"
    LIBS wtsapi32 userenv)
