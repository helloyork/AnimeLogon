add_executable(animelogon_tests common_tests.cpp clock_tests.cpp skin_tests.cpp wake_tests.cpp data_tests.cpp
                               theme_tests.cpp resolve_tests.cpp)
target_sources(animelogon_tests PRIVATE image_tests.cpp package_tests.cpp)
# The overlay's plan and its reading of still wallpapers need nothing but animelogon_common.
target_sources(animelogon_tests PRIVATE overlay_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/plan.cpp
                                        ${PROJECT_SOURCE_DIR}/overlay/src/picture.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
target_link_libraries(animelogon_tests PRIVATE animelogon_common)
add_test(NAME animelogon_tests COMMAND animelogon_tests)
# The whole run takes seconds. A test stuck in a graphics driver or in Media Foundation is ended
# after ten minutes and counts as a failure, rather than holding a CI runner for hours.
set_tests_properties(animelogon_tests PROPERTIES TIMEOUT 600)

# The settings app's elevated commands, importer and exporter, run against scratch directories.
target_sources(animelogon_tests PRIVATE config_tests.cpp
    ${PROJECT_SOURCE_DIR}/config/src/commit.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/config/src)
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

# The new settings app's library and its messages, against stores in memory.
target_sources(animelogon_tests PRIVATE app_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/json.cpp
                                        ${PROJECT_SOURCE_DIR}/app/src/library.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/app/src)

# The settings app's pictures of themes: the wall of displays when spanning, and the components
# alone as a layer, drawn by the logon screen's renderer.
target_sources(animelogon_tests PRIVATE app_picture_tests.cpp ${PROJECT_SOURCE_DIR}/app/src/pictures.cpp)
target_link_libraries(animelogon_tests PRIVATE animelogon_render)
