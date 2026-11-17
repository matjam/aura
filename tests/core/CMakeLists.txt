add_executable(test_heap test_heap.c)
target_link_libraries(test_heap aura_engine)
add_test(NAME core_heap COMMAND test_heap)

add_executable(test_atoms test_atoms.c)
target_link_libraries(test_atoms aura_engine)
add_test(NAME core_atoms COMMAND test_atoms)

add_executable(test_fastmem test_fastmem.c)
target_compile_options(test_fastmem PRIVATE -fno-builtin)
add_test(NAME core_fastmem COMMAND test_fastmem)

add_executable(test_hash test_hash.c)
target_link_libraries(test_hash aura_engine)
add_test(NAME core_hash COMMAND test_hash)
