# "install" is a reserved CMake target name, hence OUTPUT_NAME.
animelogon_add_executable(installer WIN32 ADMIN
    OUTPUT_NAME install
    SOURCES src/install.cpp
    DESCRIPTION "AnimeLogon 安装程序")

animelogon_add_executable(uninstaller ADMIN
    OUTPUT_NAME uninstall
    SOURCES src/uninstall.cpp
    DESCRIPTION "AnimeLogon 卸载程序")
