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

# localStorage and sessionStorage (src/net/web_storage.c through the
# browser): persistence, origins, tabs and the storage event, the tab's
# session storage, third-party frames, the cookie preferences, quotas,
# strings, named properties, files (damaged and truncated), clearing;
# document.referrer.
add_executable(test_web_storage test_web_storage.c ${CMAKE_SOURCE_DIR}/tests/net/tls_fixtures.c)
target_link_libraries(test_web_storage aura_fakenet m)
target_include_directories(test_web_storage PRIVATE ${CMAKE_SOURCE_DIR}/tests/net ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME webapi_web_storage COMMAND test_web_storage)
set_tests_properties(webapi_web_storage PROPERTIES TIMEOUT 120)

# document.cookie: response and script cookies read back by script, in the
# page and in frames.
add_executable(test_doc_cookie test_doc_cookie.c)
target_link_libraries(test_doc_cookie aura_fakenet m)
target_include_directories(test_doc_cookie PRIVATE ${CMAKE_SOURCE_DIR}/tests/net ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME webapi_doc_cookie COMMAND test_doc_cookie)
set_tests_properties(webapi_doc_cookie PROPERTIES TIMEOUT 120)
