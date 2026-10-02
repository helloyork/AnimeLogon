# Developer tools; not installed.
add_executable(transcode_cli transcode_cli.cpp ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_link_libraries(transcode_cli PRIVATE animelogon_common mfplat mfreadwrite mfuuid ole32)

# The settings app's importer and exporter without its window, for testing them on a machine.
add_executable(import_cli import_cli.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_link_libraries(import_cli PRIVATE animelogon_common mfplat mfreadwrite mfuuid ole32)

add_executable(skin_preview skin_preview.cpp)
target_link_libraries(skin_preview PRIVATE animelogon_render ole32)

add_executable(theme_fixture theme_fixture.cpp)
target_link_libraries(theme_fixture PRIVATE animelogon_common bcrypt)
