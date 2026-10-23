add_executable(test_platform test_platform.c)
target_link_libraries(test_platform PRIVATE plat)
target_include_directories(test_platform PRIVATE ${PROJECT_SOURCE_DIR}/platform/common)
add_test(NAME platform COMMAND test_platform)
