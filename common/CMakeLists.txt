# Code shared by every program: paths, logs, settings, themes and their stores, and machine state.
add_library(animelogon_common STATIC
    src/background.cpp
    src/bitmap.cpp
    src/blur.cpp
    src/clock.cpp
    src/components.cpp
    src/data.cpp
    src/i18n.cpp
    src/image.cpp
    src/instance.cpp
    src/live.cpp
    src/log.cpp
    src/machine.cpp
    src/monitors.cpp
    src/others.cpp
    src/package.cpp
    src/paths.cpp
    src/resolve.cpp
    src/scene.cpp
    src/secure.cpp
    src/settings.cpp
    src/sha256.cpp
    src/skin.cpp
    src/spectrum.cpp
    src/text.cpp
    src/theme.cpp
    src/wallpaper.cpp
    src/xml.cpp)
target_include_directories(animelogon_common PUBLIC include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(animelogon_common PUBLIC advapi32 bcrypt ole32 oleaut32 shell32 taskschd user32 windowscodecs)
