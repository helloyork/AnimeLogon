add_executable(animelogon_tests common_tests.cpp clock_tests.cpp skin_tests.cpp wake_tests.cpp
                               theme_tests.cpp resolve_tests.cpp)
target_sources(animelogon_tests PRIVATE image_tests.cpp package_tests.cpp)
# The settings app's elevated commands, importer and exporter, run against scratch directories.
target_sources(animelogon_tests PRIVATE config_tests.cpp
    ${PROJECT_SOURCE_DIR}/config/src/commit.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src ${PROJECT_SOURCE_DIR}/config/src)
# The transcoder is linked but never called by the tests; delay-loading keeps them starting on
# a machine without Media Foundation.
target_link_libraries(animelogon_tests PRIVATE animelogon_common mfplat mfreadwrite mfuuid ole32 delayimp)
target_link_options(animelogon_tests PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
add_test(NAME animelogon_tests COMMAND animelogon_tests)
