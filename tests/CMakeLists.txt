add_executable(animelogon_tests common_tests.cpp clock_tests.cpp skin_tests.cpp wake_tests.cpp
                               theme_tests.cpp resolve_tests.cpp)
target_sources(animelogon_tests PRIVATE image_tests.cpp package_tests.cpp)
# The overlay's plan and its reading of still wallpapers need nothing but animelogon_common.
target_sources(animelogon_tests PRIVATE overlay_tests.cpp ${PROJECT_SOURCE_DIR}/overlay/src/plan.cpp
                                        ${PROJECT_SOURCE_DIR}/overlay/src/picture.cpp)
target_include_directories(animelogon_tests PRIVATE ${PROJECT_SOURCE_DIR}/overlay/src)
target_link_libraries(animelogon_tests PRIVATE animelogon_common)
add_test(NAME animelogon_tests COMMAND animelogon_tests)
