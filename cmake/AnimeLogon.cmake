# animelogon_add_executable(<target> SOURCES <src>... DESCRIPTION <text>
#                           [WIN32] [ADMIN] [OUTPUT_NAME <name>] [LIBS <lib>...])
#
# A shipping executable: version resource, manifest and execution level included.
function(animelogon_add_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "WIN32;ADMIN" "DESCRIPTION;OUTPUT_NAME" "SOURCES;LIBS")
    if(NOT arg_OUTPUT_NAME)
        set(arg_OUTPUT_NAME ${target})
    endif()

    set(ANIMELOGON_FILE "${arg_OUTPUT_NAME}.exe")
    set(ANIMELOGON_DESC "${arg_DESCRIPTION}")
    configure_file(${PROJECT_SOURCE_DIR}/res/version.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc @ONLY)

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

    add_executable(${target} ${subsystem} ${arg_SOURCES}
                   ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc ${manifests})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${arg_OUTPUT_NAME})
    target_include_directories(${target} PRIVATE ${PROJECT_BINARY_DIR}/include)
    target_link_libraries(${target} PRIVATE ${arg_LIBS})
endfunction()
