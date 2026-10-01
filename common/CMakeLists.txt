# Code shared by every program: paths, logs, settings, the video library and machine state.
add_library(animelogon_common STATIC
    src/background.cpp
    src/clock.cpp
    src/instance.cpp
    src/library.cpp
    src/log.cpp
    src/machine.cpp
    src/monitors.cpp
    src/paths.cpp
    src/secure.cpp
    src/settings.cpp
    src/text.cpp)
target_include_directories(animelogon_common PUBLIC include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(animelogon_common PUBLIC advapi32 bcrypt ole32 shell32 user32)
