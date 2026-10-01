animelogon_add_executable(config WIN32
    SOURCES
        src/config.cpp
        src/commit.cpp
        src/transcode.cpp
        src/devices.cpp
        src/fonts.cpp
    DESCRIPTION "AnimeLogon 设置"
    LIBS animelogon_render micula::micula mfplat mfreadwrite mfuuid mmdevapi ole32 shell32 delayimp)

# Windows N has no Media Foundation without the Media Feature Pack. Delay-loaded, so the
# settings app still starts there and the transcoder can say what is missing.
target_link_options(config PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
