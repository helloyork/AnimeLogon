# animelogon_compile_shader(<target> <hlsl> <entry> <profile> <symbol> <header>)
#
# Compiles one HLSL entry point to a C header holding a byte array named <symbol>, using the
# fxc that ships with the Windows SDK. The header lands in the build directory; the target
# gets it as a source and its directory on the include path.
function(animelogon_compile_shader target hlsl entry profile symbol header)
    if(NOT ANIMELOGON_FXC)
        # Prefer the fxc from the SDK this build targets; fall back to the newest installed.
        set(_hints)
        if(DEFINED ENV{WindowsSdkVerBinPath})
            list(APPEND _hints "$ENV{WindowsSdkVerBinPath}x64")
        endif()
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
            list(APPEND _hints "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
        endif()
        file(GLOB _kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64"
                        "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64")
        if(_kits)
            list(SORT _kits)
            list(REVERSE _kits)
            list(APPEND _hints ${_kits})
        endif()
        find_program(ANIMELOGON_FXC NAMES fxc HINTS ${_hints} DOC "Direct3D shader compiler")
    endif()
    if(NOT ANIMELOGON_FXC)
        message(FATAL_ERROR "fxc.exe not found; install the Windows SDK, or set -DANIMELOGON_FXC=<path>.")
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${header}")
    get_filename_component(out_dir "${out}" DIRECTORY)
    file(MAKE_DIRECTORY "${out_dir}")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${ANIMELOGON_FXC}" /nologo /T ${profile} /E ${entry} /O3 /Vn ${symbol}
                /Fh "${out}" "${hlsl}"
        DEPENDS "${hlsl}"
        COMMENT "fxc ${entry} -> ${header}"
        VERBATIM)
    target_sources(${target} PRIVATE "${out}")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
