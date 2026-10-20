# Drawing skins with Direct2D: on the logon screen, and in previews.
add_library(animelogon_render STATIC
    src/skinpreview.cpp
    src/skinview.cpp)
target_include_directories(animelogon_render PUBLIC include)
target_link_libraries(animelogon_render PUBLIC animelogon_common d2d1 d3d11 dwrite dxgi dxguid windowscodecs)
