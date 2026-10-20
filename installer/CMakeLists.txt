# "install" is a reserved CMake target name, hence OUTPUT_NAME. Media Foundation is loaded
# at run time (engine.cpp), so that Windows N gets a message instead of a loader error.
animelogon_add_executable(installer ADMIN
    OUTPUT_NAME install
    SOURCES src/install.cpp src/engine.cpp
    DESCRIPTION "AnimeLogon 安装程序"
    LIBS mfuuid ole32 shell32)

animelogon_add_executable(uninstaller ADMIN
    OUTPUT_NAME uninstall
    SOURCES src/uninstall.cpp src/engine.cpp
    DESCRIPTION "AnimeLogon 卸载程序"
    LIBS mfuuid ole32 shell32)
