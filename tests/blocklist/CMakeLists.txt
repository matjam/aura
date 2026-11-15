# Content blocking: the list parser, compiled files and matching
# (test_blocklist), and over the in-process fake transport of tests/net
# (test_blocklist_net): the loader refusing listed hosts without a
# connection, the blocked-site page and Visit Anyway in a browser window,
# and the update manager. No test touches the network or a real list.
add_executable(test_blocklist test_blocklist.c)
target_link_libraries(test_blocklist aura_engine m)
target_include_directories(test_blocklist PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME blocklist COMMAND test_blocklist)

add_executable(test_blocklist_net test_blocklist_net.c ${CMAKE_SOURCE_DIR}/tests/net/tls_fixtures.c)
target_link_libraries(test_blocklist_net aura_fakenet m)
target_include_directories(test_blocklist_net PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME blocklist_net COMMAND test_blocklist_net)
set_tests_properties(blocklist_net PROPERTIES TIMEOUT 120)

# The parser, the builder and the compiled-file check on random and
# mutated input. To run longer, build with the sanitizers and run
#   fuzz_blocklist ITERATIONS SEED [SECONDS]
# It stops at the iteration count or the time, and prints how far it got.
add_executable(fuzz_blocklist fuzz_blocklist.c)
target_link_libraries(fuzz_blocklist aura_engine m)
target_include_directories(fuzz_blocklist PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME blocklist_fuzz COMMAND fuzz_blocklist 3000 1 20)
