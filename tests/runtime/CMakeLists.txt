add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE swfrt)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_builtins test_builtins.c)
target_link_libraries(test_builtins PRIVATE swfrt)
target_include_directories(test_builtins PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME builtins COMMAND test_builtins)
