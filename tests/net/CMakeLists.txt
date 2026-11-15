# Network stack tests. All run against the in-process fake transport
# (fakenet.c); none touches the network. Tests that use the live network
# are in live/ and run only when AURA_NET_LIVE=1.
add_library(aura_fakenet STATIC fakenet.c)
target_link_libraries(aura_fakenet PUBLIC aura_engine)
target_include_directories(aura_fakenet PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_http test_http.c)
target_link_libraries(test_http aura_fakenet)
add_test(NAME net_http COMMAND test_http)
set_tests_properties(net_http PROPERTIES TIMEOUT 120)

add_executable(test_cookies test_cookies.c)
target_link_libraries(test_cookies aura_fakenet)
add_test(NAME net_cookies COMMAND test_cookies)

add_executable(test_tls test_tls.c tls_fixtures.c)
target_link_libraries(test_tls aura_fakenet)
add_test(NAME net_tls COMMAND test_tls)
set_tests_properties(net_tls PROPERTIES TIMEOUT 120)

add_executable(test_cache test_cache.c)
target_link_libraries(test_cache aura_fakenet)
add_test(NAME net_cache COMMAND test_cache)

add_executable(test_loader test_loader.c)
target_link_libraries(test_loader aura_fakenet)
add_test(NAME net_loader COMMAND test_loader)

add_executable(test_policy test_policy.c)
target_link_libraries(test_policy aura_engine)
add_test(NAME net_policy COMMAND test_policy)

# To run a long session, build with sanitizers and run
#   fuzz_cookies ITERATIONS SEED
add_executable(fuzz_cookies fuzz_cookies.c)
target_link_libraries(fuzz_cookies aura_engine)
add_test(NAME net_cookies_fuzz COMMAND fuzz_cookies 20000 1)

# Parsers: HTTP responses and chunked bodies, data: URLs, PSL, the root
# store loader, certificates, HSTS, authentication headers, dates, MIME.
#   fuzz_net all ITERATIONS SEED assets/net/aura-roots.bin
add_executable(fuzz_net fuzz_net.c tls_fixtures.c)
target_link_libraries(fuzz_net aura_engine)
target_include_directories(fuzz_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME net_parsers_fuzz COMMAND fuzz_net all 30000 1 ${CMAKE_SOURCE_DIR}/assets/net/aura-roots.bin)

include(${CMAKE_CURRENT_SOURCE_DIR}/more.cmake OPTIONAL)
