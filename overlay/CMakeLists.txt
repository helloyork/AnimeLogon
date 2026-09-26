# Console programs, started with CREATE_NO_WINDOW on the Winlogon desktop.
animelogon_add_executable(overlay
    SOURCES src/overlay.cpp
    DESCRIPTION "AnimeLogon 登录界面视频")

animelogon_add_executable(launcher
    SOURCES src/launcher.cpp
    DESCRIPTION "AnimeLogon 登录界面视频启动器")
