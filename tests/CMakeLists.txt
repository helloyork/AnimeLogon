add_executable(animelogon_tests common_tests.cpp clock_tests.cpp wake_tests.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
target_link_libraries(animelogon_tests PRIVATE animelogon_common)
add_test(NAME animelogon_tests COMMAND animelogon_tests)
