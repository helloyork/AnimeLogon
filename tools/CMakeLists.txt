# Developer tools; not installed.
add_executable(transcode_cli transcode_cli.cpp ${PROJECT_SOURCE_DIR}/config/src/transcode.cpp)
target_link_libraries(transcode_cli PRIVATE animelogon_common mfplat mfreadwrite mfuuid ole32)
