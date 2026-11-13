# animelogon-steam.exe: the only program that loads Steam's library, steam_api64.dll, for the
# settings app's Steam Workshop pages. It speaks JSON lines on standard input and output
# (PROTOCOL.md). Steam's library is not part of this repository: the program looks it up at run
# time in its own folder, and declares the parts of its API it uses itself (src/steam_flat.h).
#
# Valve ships steam_api64.dll for x64 Windows only, so the program is built for x64 only; on an
# ARM64 machine it runs under emulation beside the settings app built for ARM64.
if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(STATUS "animelogon-steam is not built for ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}: Steamworks has no Windows library for it")
    return()
endif()

animelogon_add_executable(animelogon_steam
    OUTPUT_NAME animelogon-steam
    SOURCES
        src/main.cpp
        src/api.cpp
        src/session.cpp
        ${PROJECT_SOURCE_DIR}/app/src/json.cpp
    DESCRIPTION "AnimeLogon Steam Workshop")
target_include_directories(animelogon_steam PRIVATE src ${PROJECT_SOURCE_DIR}/app/src)
