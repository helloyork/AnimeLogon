# Code shared by every program: paths, logs, settings, the video library and machine state.
add_library(animelogon_common STATIC
    src/background.cpp
    src/bitmap.cpp
    src/clock.cpp
    src/components.cpp
    src/image.cpp
    src/instance.cpp
    src/library.cpp
    src/log.cpp
    src/machine.cpp
    src/monitors.cpp
    src/package.cpp
    src/paths.cpp
    src/resolve.cpp
    src/secure.cpp
    src/settings.cpp
    src/sha256.cpp
    src/skin.cpp
    src/skins.cpp
    src/text.cpp
    src/theme.cpp
    src/wallpaper.cpp
    src/xml.cpp)
target_include_directories(animelogon_common PUBLIC include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(animelogon_common PUBLIC advapi32 bcrypt ole32 shell32 user32 windowscodecs)
