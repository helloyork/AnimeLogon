# cmake -DMACHINE=<hex> -DFILES=<file>|<file>... -P PeMachine.cmake
#
# Fails unless every file exists and is a PE image for MACHINE (the COFF header's Machine field:
# 8664 for x64, AA64 for ARM64). install.exe embeds another build's programs for another
# architecture; this keeps a wrong or missing one from going into it unnoticed.
string(REPLACE "|" ";" files "${FILES}")
string(TOLOWER "${MACHINE}" want)
foreach(file IN LISTS files)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} is missing: build that architecture's Release configuration first.")
    endif()
    file(READ "${file}" mz LIMIT 2 HEX)
    if(NOT mz STREQUAL "4d5a")
        message(FATAL_ERROR "${file} is not a Windows program.")
    endif()
    # e_lfanew, at offset 60, is where the "PE\0\0" signature is; the machine follows it.
    file(READ "${file}" lfanew OFFSET 60 LIMIT 4 HEX)
    string(SUBSTRING "${lfanew}" 6 2 b3)
    string(SUBSTRING "${lfanew}" 4 2 b2)
    string(SUBSTRING "${lfanew}" 2 2 b1)
    string(SUBSTRING "${lfanew}" 0 2 b0)
    math(EXPR offset "0x${b3}${b2}${b1}${b0}")
    file(READ "${file}" header OFFSET ${offset} LIMIT 6 HEX)
    string(SUBSTRING "${header}" 0 8 signature)
    string(SUBSTRING "${header}" 10 2 m1)
    string(SUBSTRING "${header}" 8 2 m0)
    if(NOT signature STREQUAL "50450000")
        message(FATAL_ERROR "${file} has no PE header.")
    endif()
    if(NOT "${m1}${m0}" STREQUAL want)
        message(FATAL_ERROR "${file} is for machine ${m1}${m0}, not ${want}.")
    endif()
endforeach()
