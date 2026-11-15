add_executable(test_heap test_heap.c)
target_link_libraries(test_heap aura_engine)
add_test(NAME core_heap COMMAND test_heap)
