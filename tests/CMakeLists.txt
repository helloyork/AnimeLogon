add_executable(animelogon_tests common_tests.cpp clock_tests.cpp skin_tests.cpp wake_tests.cpp data_tests.cpp
                               theme_tests.cpp resolve_tests.cpp motion_tests.cpp)
target_sources(animelogon_tests PRIVATE image_tests.cpp package_tests.cpp look_tests.cpp)
# The theme list the logon screen moves through, and the sign-in picture handed to the agent.
target_sources(animelogon_tests PRIVATE rotation_tests.cpp)
# The wallpaper blur on the graphics card against the same blur on the processor, on WARP.
include(${PROJECT_SOURCE_DIR}/cmake/CompileShader.cmake)
target_sources(animelogon_tests PRIVATE gpublur_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/gpublur.cpp)
foreach(entry IN ITEMS VS Box Gauss Up)
    string(TOLOWER ${entry} lower)
    if(entry STREQUAL "VS")
        set(profile vs_5_0)
    else()
        set(profile ps_5_0)
    endif()
    animelogon_compile_shader(animelogon_tests ${PROJECT_SOURCE_DIR}/overlay/shaders/blur.hlsl ${entry} ${profile}
                              g_blur_${lower} blur_${lower}.h)
endforeach()
target_sources(animelogon_tests PRIVATE scenegraph_tests.cpp)
target_sources(animelogon_tests PRIVATE scene_runtime_tests.cpp)
# Scene textures: the file format, and the encoder the sandboxed renderer runs at import, on WARP.
target_sources(animelogon_tests PRIVATE texture_tests.cpp ${PROJECT_SOURCE_DIR}/scene/src/texencode.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/scene/src)
target_link_libraries(animelogon_tests PRIVATE directxtex)
add_dependencies(animelogon_tests scene)
# The scene importer and renderer that scene.exe runs, here on WARP, compared pixel by pixel.
target_sources(animelogon_tests PRIVATE scene_render_tests.cpp ${PROJECT_SOURCE_DIR}/scene/src/renderer.cpp ${PROJECT_SOURCE_DIR}/scene/src/contract.cpp ${PROJECT_SOURCE_DIR}/scene/src/effects.cpp ${PROJECT_SOURCE_DIR}/scene/src/particles.cpp
                                        ${PROJECT_SOURCE_DIR}/scene/src/import.cpp ${PROJECT_SOURCE_DIR}/scene/src/lottie.cpp)
target_include_directories(animelogon_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/vendor/thorvg/src/common
    ${PROJECT_SOURCE_DIR}/vendor/thorvg/src/renderer
    ${PROJECT_BINARY_DIR}/vendor/thorvg/config)
target_link_libraries(animelogon_tests PRIVATE thorvg d3dcompiler windowscodecs)
# The overlay's plan and its reading of still wallpapers need nothing but animelogon_common.
target_sources(animelogon_tests PRIVATE overlay_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/plan.cpp
                                        ${PROJECT_SOURCE_DIR}/overlay/src/picture.cpp)
# Keeping off Windows' shutdown, restart and update screens.
target_sources(animelogon_tests PRIVATE shutdown_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/screen.cpp
                                        ${PROJECT_SOURCE_DIR}/overlay/src/winlogonwatch.cpp)
target_link_libraries(animelogon_tests PRIVATE wtsapi32 advapi32)
# The lock screen on several displays: layout changes, the password box's display, the span blur,
# the wake transition across a wall, and no display at all (header-only pieces, screen.cpp, plan.cpp).
target_sources(animelogon_tests PRIVATE display_tests.cpp)
# Giving way to Windows: the watchdog's rule, the launcher's reading of exit codes and of "Switch
# user", a pen or a touch waking the screen, and the parked overlay's planning (header-only).
target_sources(animelogon_tests PRIVATE runtime_tests.cpp)
# The wallpaper's sound track giving way to other programs' sound, and not to one that only repeats
# it (header-only).
target_sources(animelogon_tests PRIVATE soundyield_tests.cpp)
# Which of the other displays get a still after a wake, what each is cut from, and the blur.
target_sources(animelogon_tests PRIVATE stills_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/stills.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
# The installer taking back an uninstall's deletions at the next restart.
target_sources(animelogon_tests PRIVATE restart_tests.cpp ${PROJECT_SOURCE_DIR}/installer/src/restart.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/installer/src)
# What install.exe carries against the list it was made from (installer/CMakeLists.txt), read out
# of the install.exe this build made, and which architecture a machine gets.
target_sources(animelogon_tests PRIVATE payload_tests.cpp ${PROJECT_SOURCE_DIR}/installer/src/payload.cpp)
target_include_directories(animelogon_tests PRIVATE ${ANIMELOGON_PAYLOAD_INCLUDE})
add_dependencies(animelogon_tests installer)
target_compile_definitions(animelogon_tests PRIVATE "ANIMELOGON_INSTALL_EXE=\"$<TARGET_FILE:installer>\"")
# The bundled wallpaper's video, which player_tests.cpp checks against what install.exe says of it.
target_compile_definitions(animelogon_tests PRIVATE
    "ANIMELOGON_BUNDLED_WALLPAPER=\"${PROJECT_SOURCE_DIR}/res/wallpapers/soft-hills.mp4\"")
target_link_libraries(animelogon_tests PRIVATE animelogon_common)
# The strings, read in Simplified Chinese (common_tests.cpp's main), which the tests expect.
animelogon_locales_rc(test_locales)
target_sources(animelogon_tests PRIVATE ${test_locales})
add_test(NAME animelogon_tests COMMAND animelogon_tests)
# The whole run takes seconds. A test stuck in a graphics driver or in Media Foundation is ended
# after ten minutes and counts as a failure, rather than holding a CI runner for hours.
set_tests_properties(animelogon_tests PROPERTIES TIMEOUT 600)

# The settings app's elevated commands, importer and exporter, run against scratch directories.
target_sources(animelogon_tests PRIVATE config_tests.cpp
    ${PROJECT_SOURCE_DIR}/config/src/commit.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/sceneimport.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/config/src)
# Where the library is: the drives an install may go on, and the stores following the library.
target_sources(animelogon_tests PRIVATE library_tests.cpp)
# Only export_sound_tests.cpp and player_tests.cpp call into Media Foundation, and they check
# first; delay-loading keeps the tests starting on a machine without Media Foundation.
target_link_libraries(animelogon_tests PRIVATE mfplat mfreadwrite mfuuid ole32 delayimp)
target_link_options(animelogon_tests PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)

# Exporting a video wallpaper with its sound and importing it again, with Media Foundation for
# real on a clip made by the test.
target_sources(animelogon_tests PRIVATE export_sound_tests.cpp ${PROJECT_SOURCE_DIR}/config/src/remux.cpp)

# The overlay's video player decoding clips made by the test, in software on a WARP device and,
# where there is one, on a hardware device with video support.
target_sources(animelogon_tests PRIVATE player_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/player.cpp)
target_link_libraries(animelogon_tests PRIVATE d3d11)

# The new settings app's library and its messages, and what the icon in the notification area
# offers, against stores in memory.
target_sources(animelogon_tests PRIVATE app_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/json.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/library.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/traymenu.cpp)
# Finding Wallpaper Engine's video wallpapers, in folders the test lays out as Steam does.
target_sources(animelogon_tests PRIVATE workshop_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/workshop.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/app/src)

# The settings app's pictures of themes: the wall of displays when spanning, and the components
# alone as a layer, drawn by the logon screen's renderer.
target_sources(animelogon_tests PRIVATE app_picture_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/pictures.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/livespectrum.cpp)
target_link_libraries(animelogon_tests PRIVATE animelogon_render animelogon_scene_host)

# Finding an app's files and Workshop downloads in Steam's libraries (workshop.cpp uses it too).
target_sources(animelogon_tests PRIVATE steam_library_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/steam_library.cpp)

# The Steam helper (steam/) run as a child process against a stand-in for Steam's library: a DLL
# named steam_api64.dll that exports the entry points the helper looks up and answers from a JSON
# fixture (steam/PROTOCOL.md). Built only where the helper is (x64), into a folder of its own, so
# that it is never mistaken for Valve's library or copied beside the programs in bin/.
if(TARGET animelogon_steam)
    add_library(fake_steam_api SHARED fake_steam/fake_steam_api.cpp ${PROJECT_SOURCE_DIR}/app/src/json.cpp)
    set_target_properties(fake_steam_api PROPERTIES OUTPUT_NAME steam_api64
                          RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/fake-steam)
    target_include_directories(fake_steam_api PRIVATE ${PROJECT_SOURCE_DIR}/steam/src ${PROJECT_SOURCE_DIR}/app/src)
    target_link_libraries(fake_steam_api PRIVATE animelogon_common)
    target_sources(animelogon_tests PRIVATE steam_tests.cpp)
    add_dependencies(animelogon_tests animelogon_steam fake_steam_api)
    target_compile_definitions(animelogon_tests PRIVATE
        "ANIMELOGON_STEAM_HELPER=\"$<TARGET_FILE:animelogon_steam>\""
        "ANIMELOGON_FAKE_STEAM_DLL=\"$<TARGET_FILE:fake_steam_api>\"")
endif()

# The library's Steam Workshop items (workshop.ini, an item's folder, its state, what an update may
# take away with the theme it replaces) and the settings app's cutting of the Steam helper's output
# into lines. steam/src only for the app id the client starts the helper with.
target_sources(animelogon_tests PRIVATE steam_workshop_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/steam_workshop.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/steam_client.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/steam/src)
# The settings app's client of the Steam helper, run against the helper and the stand-in library.
if(TARGET animelogon_steam)
    target_sources(animelogon_tests PRIVATE steam_client_tests.cpp)
endif()

# The audio spectrum: its bands read out of sound the test makes, the format's <spectrum>, and
# drawing it with the logon screen's renderer; and the logon screen listening to the output device,
# where the machine has one.
target_sources(animelogon_tests PRIVATE spectrum_tests.cpp)

# What the installer registers so that .altheme files open in AnimeLogon, against a registry in
# memory, and the settings app's reading of its command line.
target_sources(animelogon_tests PRIVATE assoc_tests.cpp ${PROJECT_SOURCE_DIR}/installer/src/assoc.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/arguments.cpp)
