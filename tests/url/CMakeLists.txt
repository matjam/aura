# URL parser tests. The WPT data is optional: to fetch it, run
#   mkdir -p build/wpt-url && curl -o build/wpt-url/urltestdata.json \
#     https://raw.githubusercontent.com/web-platform-tests/wpt/master/url/resources/urltestdata.json
# The WPT test skips (exit 77) when the file is absent; inputs listed in
# wpt-expected-failures.txt may fail (each with its reason in the file's
# comments), any other failure fails the test.
add_executable(test_url test_url.c ${CMAKE_SOURCE_DIR}/tests/html/json.c)
target_link_libraries(test_url aura_engine)
add_test(NAME url COMMAND test_url)
add_test(NAME url_wpt COMMAND test_url --wpt ${CMAKE_SOURCE_DIR}/build/wpt-url/urltestdata.json
                              ${CMAKE_CURRENT_SOURCE_DIR}/wpt-expected-failures.txt)
set_tests_properties(url_wpt PROPERTIES SKIP_RETURN_CODE 77)

add_executable(fuzz_url fuzz_url.c)
target_link_libraries(fuzz_url aura_engine)
add_test(NAME url_fuzz COMMAND fuzz_url 30000 1)
