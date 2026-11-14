# Developer tools; not installed.
add_executable(transcode_cli transcode_cli.cpp ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
animelogon_locales_rc(tool_locales)
target_sources(transcode_cli PRIVATE ${tool_locales})
target_link_libraries(transcode_cli PRIVATE animelogon_common mfplat mfreadwrite mfuuid ole32)

# The settings app's importer and exporter without its window, for testing them on a machine.
add_executable(import_cli import_cli.cpp
    ${PROJECT_SOURCE_DIR}/config/src/exporter.cpp
    ${PROJECT_SOURCE_DIR}/config/src/remux.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/sceneimport.cpp
    ${PROJECT_SOURCE_DIR}/config/src/store.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_link_libraries(import_cli PRIVATE animelogon_common animelogon_scene_host mfplat mfreadwrite mfuuid ole32)
# The importer's messages come from res/locales.
animelogon_locales_rc(tool_locales)
target_sources(import_cli PRIVATE ${tool_locales})

add_executable(skin_preview skin_preview.cpp)
target_link_libraries(skin_preview PRIVATE animelogon_render ole32)

add_executable(theme_fixture theme_fixture.cpp)
add_executable(scene_preview scene_preview.cpp
    ${PROJECT_SOURCE_DIR}/config/src/staging.cpp
    ${PROJECT_SOURCE_DIR}/config/src/sceneimport.cpp
    ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_link_libraries(scene_preview PRIVATE animelogon_scene_preview mfplat mfreadwrite mfuuid ole32)
target_sources(scene_preview PRIVATE ${tool_locales})
target_link_libraries(theme_fixture PRIVATE animelogon_common bcrypt)

# Pictures of every page of the installer's and uninstaller's wizards, without installing.
animelogon_add_executable(setup_preview WIN32 LOCALES
    SOURCES setup_preview.cpp
        ${PROJECT_SOURCE_DIR}/installer/src/ui.cpp
        ${PROJECT_SOURCE_DIR}/installer/src/wizards.cpp
        ${PROJECT_SOURCE_DIR}/installer/src/engine.cpp
        ${PROJECT_SOURCE_DIR}/installer/src/payload.cpp
        ${PROJECT_SOURCE_DIR}/installer/src/restart.cpp
    DESCRIPTION "AnimeLogon Setup Preview"
    LIBS mfuuid ole32 oleaut32 shell32 comctl32 d2d1 dwrite dwmapi)
# A console program, though its windows need the manifest of a windowed one (common controls 6).
set_target_properties(setup_preview PROPERTIES WIN32_EXECUTABLE OFF)
# The installer's list of what it installs (installer/CMakeLists.txt).
target_include_directories(setup_preview PRIVATE ${ANIMELOGON_PAYLOAD_INCLUDE})
