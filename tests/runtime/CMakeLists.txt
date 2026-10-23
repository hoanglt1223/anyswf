add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE swfrt)
add_test(NAME runtime COMMAND test_runtime)
