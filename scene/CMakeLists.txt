add_library(animelogon_scene_host STATIC src/host.cpp)
target_include_directories(animelogon_scene_host PUBLIC include PRIVATE src)
target_link_libraries(animelogon_scene_host PUBLIC animelogon_common d3d11 dxgi dxguid userenv)
animelogon_add_executable(scene SOURCES src/main.cpp src/renderer.cpp DESCRIPTION "AnimeLogon 场景渲染器"
    LIBS d3d11 dxgi dxguid d3dcompiler)
target_include_directories(scene PRIVATE include src)
