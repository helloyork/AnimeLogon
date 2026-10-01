cmake_minimum_required(VERSION 3.25)

project(AnimeLogon VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "AnimeLogon builds with MSVC.")
endif()

set(ANIMELOGON_BUILD 0 CACHE STRING "Build number in the version resource; CI passes its run number")
set(ANIMELOGON_VERSION_SUFFIX "" CACHE STRING "Pre-release suffix, for example -beta1")

set(ANIMELOGON_VERSION "${PROJECT_VERSION}${ANIMELOGON_VERSION_SUFFIX}")
set(ANIMELOGON_DISPLAY_NAME "AnimeLogon")
set(ANIMELOGON_COMPANY "Nomen (helloyork)")
set(ANIMELOGON_COPYRIGHT "© 2026 Nomen (helloyork)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: these programs run on the logon screen, or before anything is installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# PDBs for release builds too, so crash offsets can be symbolized.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>")
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
# Execution levels come from res/*.manifest, so the linker must not add its own.
add_link_options(/MANIFESTUAC:NO "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")

configure_file(res/version.h.in ${PROJECT_BINARY_DIR}/include/animelogon/version.h @ONLY)

add_subdirectory(vendor/micula EXCLUDE_FROM_ALL)

include(cmake/AnimeLogon.cmake)

add_subdirectory(common)
add_subdirectory(render)
add_subdirectory(overlay)
add_subdirectory(config)
add_subdirectory(installer)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(tools)
endif()
