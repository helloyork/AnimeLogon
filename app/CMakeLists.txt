# animelogon.exe, the settings app: a WebView2 page over the stores. The importer, the exporter
# and the elevated helper are the ones config.exe has (config/src), with the same command line.
set(APP_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
configure_file(src/web.rc.in ${CMAKE_CURRENT_BINARY_DIR}/web.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/web.rc PROPERTIES
    OBJECT_DEPENDS "${APP_WEB_DIR}/index.html;${APP_WEB_DIR}/app.css;${APP_WEB_DIR}/app.js")

set(config_src ${PROJECT_SOURCE_DIR}/config/src)
animelogon_add_executable(app WIN32
    OUTPUT_NAME animelogon
    SOURCES
        src/main.cpp
        src/host.cpp
        src/library.cpp
        src/pictures.cpp
        src/json.cpp
        src/resources.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/web.rc
        ${config_src}/commit.cpp
        ${config_src}/store.cpp
        ${config_src}/staging.cpp
        ${config_src}/exporter.cpp
        ${config_src}/remux.cpp
        ${config_src}/transcode.cpp
        ${config_src}/devices.cpp
        ${config_src}/fonts.cpp
    DESCRIPTION "AnimeLogon 设置"
    LIBS animelogon_render webview2::loader mfplat mfreadwrite mfuuid mmdevapi ole32 shell32 shlwapi comdlg32
         dwmapi delayimp)
target_include_directories(app PRIVATE src ${config_src})
# Windows N has no Media Foundation without the Media Feature Pack, and the elevated helper needs
# no browser: both are loaded only when used, so the app can say what is missing.
target_link_options(app PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:WebView2Loader.dll)
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:webview2::loader> $<TARGET_FILE_DIR:app>)
