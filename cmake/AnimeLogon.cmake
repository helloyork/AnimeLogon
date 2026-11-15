# animelogon_add_executable(<target> SOURCES <src>... DESCRIPTION <text>
#                           [WIN32] [ADMIN] [LOCALES] [OUTPUT_NAME <name>] [LIBS <lib>...])
#
# A shipping executable: version resource, program icon, manifest and execution level included.
# LOCALES adds the strings of every language (res/locales), for programs that show any.
function(animelogon_add_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "WIN32;ADMIN;LOCALES" "DESCRIPTION;OUTPUT_NAME" "SOURCES;LIBS")
    if(NOT arg_OUTPUT_NAME)
        set(arg_OUTPUT_NAME ${target})
    endif()

    set(ANIMELOGON_FILE "${arg_OUTPUT_NAME}.exe")
    set(ANIMELOGON_DESC "${arg_DESCRIPTION}")
    set(ANIMELOGON_ICON "${PROJECT_SOURCE_DIR}/res/animelogon.ico")
    configure_file(${PROJECT_SOURCE_DIR}/res/version.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc @ONLY)
    # The resource compiler reads the icon file, but the build does not see it as an input on its own.
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${target}.rc PROPERTIES
                                OBJECT_DEPENDS ${ANIMELOGON_ICON})

    set(manifests ${PROJECT_SOURCE_DIR}/res/app.manifest)
    if(arg_ADMIN)
        list(APPEND manifests ${PROJECT_SOURCE_DIR}/res/admin.manifest)
    else()
        list(APPEND manifests ${PROJECT_SOURCE_DIR}/res/invoker.manifest)
    endif()

    set(subsystem)
    if(arg_WIN32)
        set(subsystem WIN32)
        list(APPEND manifests ${PROJECT_SOURCE_DIR}/res/window.manifest)
    endif()

    set(locales)
    if(arg_LOCALES)
        animelogon_locales_rc(locales)
    endif()

    add_executable(${target} ${subsystem} ${arg_SOURCES}
                   ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc ${manifests} ${locales})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${arg_OUTPUT_NAME})
    target_include_directories(${target} PRIVATE ${PROJECT_BINARY_DIR}/include)
    target_link_libraries(${target} PRIVATE animelogon_common ${arg_LIBS})
endfunction()

# animelogon_locales_rc(<out-var>)
#
# The resource script that carries res/locales/*.json, configured once into the build directory,
# with the JSON files as its inputs so that a changed translation relinks what carries it.
function(animelogon_locales_rc out)
    set(rc ${PROJECT_BINARY_DIR}/locales.rc)
    if(NOT EXISTS ${rc})
        set(ANIMELOGON_LOCALES ${PROJECT_SOURCE_DIR}/res/locales)
        configure_file(${PROJECT_SOURCE_DIR}/res/locales.rc.in ${rc} @ONLY)
    endif()
    file(GLOB jsons ${PROJECT_SOURCE_DIR}/res/locales/*.json)
    set_source_files_properties(${rc} PROPERTIES OBJECT_DEPENDS "${jsons}")
    set(${out} ${rc} PARENT_SCOPE)
endfunction()
