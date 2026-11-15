add_executable(test_heap test_heap.c)
target_link_libraries(test_heap aura_engine)
add_test(NAME core_heap COMMAND test_heap)

add_executable(test_hash test_hash.c)
target_link_libraries(test_hash aura_engine)
add_test(NAME core_hash COMMAND test_hash)
