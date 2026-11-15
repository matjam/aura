# Script requests in a browser window over the fake transport of tests/net
# (no network): fetch and XMLHttpRequest methods, headers and bodies, the
# CORS protocol (preflight, checks, exposure, redirects, credentials),
# opaque responses, Cross-Origin-Resource-Policy, crossorigin elements.
add_executable(test_cors_net test_cors_net.c)
target_link_libraries(test_cors_net aura_fakenet m)
target_include_directories(test_cors_net PRIVATE ${CMAKE_SOURCE_DIR}/tests/net ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME webapi_cors_net COMMAND test_cors_net)
set_tests_properties(webapi_cors_net PROPERTIES TIMEOUT 120)

# The CORS rules as functions (src/net/cors.c) and the digests.
add_executable(test_cors test_cors.c)
target_link_libraries(test_cors aura_engine m)
target_include_directories(test_cors PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME webapi_cors COMMAND test_cors)
