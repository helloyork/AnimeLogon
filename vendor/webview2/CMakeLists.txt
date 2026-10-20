# Microsoft's WebView2 SDK: the headers and the loader DLL for the build's architecture.
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" webview2_arch)
if(NOT webview2_arch MATCHES "^(x64|arm64)$")
    message(FATAL_ERROR "No WebView2 loader for architecture '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'.")
endif()

add_library(webview2_loader SHARED IMPORTED GLOBAL)
set_target_properties(webview2_loader PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/${webview2_arch}/WebView2Loader.dll
    IMPORTED_IMPLIB ${CMAKE_CURRENT_SOURCE_DIR}/${webview2_arch}/WebView2Loader.dll.lib
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(webview2::loader ALIAS webview2_loader)
