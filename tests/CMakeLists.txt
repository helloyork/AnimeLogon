add_executable(animelogon_tests common_tests.cpp clock_tests.cpp skin_tests.cpp wake_tests.cpp
                               theme_tests.cpp resolve_tests.cpp)
target_sources(animelogon_tests PRIVATE image_tests.cpp package_tests.cpp)
# The overlay's plan and its reading of still wallpapers need nothing but animelogon_common.
target_sources(animelogon_tests PRIVATE overlay_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/plan.cpp
                                        ${PROJECT_SOURCE_DIR}/overlay/src/picture.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
target_link_libraries(animelogon_tests PRIVATE animelogon_common)
add_test(NAME animelogon_tests COMMAND animelogon_tests)

# The settings app's elevated commands, importer and exporter, run against scratch directories.
target_sources(animelogon_tests PRIVATE config_tests.cpp
    ${PROJECT_SOURCE_DIR}/config/src/commit.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/config/src)
# Only export_sound_tests.cpp calls into Media Foundation, and it checks first; delay-loading
# keeps the tests starting on a machine without Media Foundation.
target_link_libraries(animelogon_tests PRIVATE mfplat mfreadwrite mfuuid ole32 delayimp)
target_link_options(animelogon_tests PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)

# Exporting a video wallpaper with its sound and importing it again, with Media Foundation for
# real on a clip made by the test.
target_sources(animelogon_tests PRIVATE export_sound_tests.cpp ${PROJECT_SOURCE_DIR}/config/src/remux.cpp)
