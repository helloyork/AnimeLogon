# animelogon.exe, the settings app: a WebView2 page over the stores. The importer, the exporter
# and the elevated helper's commands are in config/src.

# The page is React with Fluent UI (app/web), bundled by esbuild into index.html, app.css and
# app.js in the build tree, which the program carries as resources. It needs Node.js and npm.
# The packages are installed without running their install scripts: none of them needs one.
find_program(NODE_EXECUTABLE node REQUIRED)
find_program(NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
set(APP_WEB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(APP_WEB_DIR ${CMAKE_CURRENT_BINARY_DIR}/web)
file(GLOB_RECURSE app_web_sources CONFIGURE_DEPENDS ${APP_WEB_SRC}/src/*.ts ${APP_WEB_SRC}/src/*.tsx)
add_custom_command(OUTPUT ${APP_WEB_SRC}/node_modules/.package-lock.json
    # npm is a .cmd script: called through cmd so that the build step goes on after it.
    COMMAND cmd /c npm ci --ignore-scripts --no-audit --no-fund
    DEPENDS ${APP_WEB_SRC}/package.json ${APP_WEB_SRC}/package-lock.json
    WORKING_DIRECTORY ${APP_WEB_SRC}
    COMMENT "Installing the settings page's packages"
    VERBATIM)
add_custom_command(OUTPUT ${APP_WEB_DIR}/index.html ${APP_WEB_DIR}/app.css ${APP_WEB_DIR}/app.js
    COMMAND ${NODE_EXECUTABLE} build.mjs --out ${APP_WEB_DIR}
    DEPENDS ${app_web_sources} ${APP_WEB_SRC}/build.mjs ${APP_WEB_SRC}/index.html ${APP_WEB_SRC}/app.css
            ${APP_WEB_SRC}/node_modules/.package-lock.json
    WORKING_DIRECTORY ${APP_WEB_SRC}
    COMMENT "Bundling the settings page"
    VERBATIM)
add_custom_target(app_web DEPENDS ${APP_WEB_DIR}/index.html ${APP_WEB_DIR}/app.css ${APP_WEB_DIR}/app.js)
configure_file(src/web.rc.in ${CMAKE_CURRENT_BINARY_DIR}/web.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/web.rc PROPERTIES
    OBJECT_DEPENDS "${APP_WEB_DIR}/index.html;${APP_WEB_DIR}/app.css;${APP_WEB_DIR}/app.js")

set(config_src ${PROJECT_SOURCE_DIR}/config/src)
animelogon_add_executable(app WIN32
    OUTPUT_NAME animelogon
    SOURCES
        src/main.cpp
        src/agent.cpp
        src/host.cpp
        src/library.cpp
        src/pictures.cpp
        src/json.cpp
        src/resources.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/web.rc
        ${config_src}/commit.cpp
        ${config_src}/store.cpp
        ${config_src}/staging.cpp
        ${config_src}/sceneimport.cpp
        ${config_src}/exporter.cpp
        ${config_src}/remux.cpp
        ${config_src}/transcode.cpp
        ${config_src}/devices.cpp
        ${config_src}/fonts.cpp
    DESCRIPTION "AnimeLogon 设置"
    LIBS animelogon_render animelogon_scene_preview webview2::loader mfplat mfreadwrite mfuuid mmdevapi ole32 shell32 shlwapi comdlg32
         dwmapi delayimp windowsapp)
target_include_directories(app PRIVATE src ${config_src})
add_dependencies(app app_web)
# The agent reads Windows' media controls through C++/WinRT, which needs C++20's <coroutine>:
# under C++17 it falls back to <experimental/coroutine>, which newer MSVC refuses to compile.
set_target_properties(app PROPERTIES CXX_STANDARD 20)
# Windows N has no Media Foundation without the Media Feature Pack, and the elevated helper needs
# no browser: both are loaded only when used, so the app can say what is missing.
target_link_options(app PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:WebView2Loader.dll)
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:webview2::loader> $<TARGET_FILE_DIR:app>)
