add_executable(test_html_tokenizer test_tokenizer.c json.c)
target_link_libraries(test_html_tokenizer aura_engine)

add_executable(test_html_tree test_tree.c)
target_link_libraries(test_html_tree aura_engine)

add_executable(test_html_api test_html_api.c)
target_link_libraries(test_html_api aura_engine)

# API, serializer, encoding, script and scaling tests.
add_test(NAME html_api COMMAND test_html_api)

# The <meta charset> prescan (src/html/input.c), with a short fuzz run;
# pass a larger count for a long session.
add_executable(test_html_prescan test_prescan.c)
target_link_libraries(test_html_prescan aura_engine)
add_test(NAME html_prescan COMMAND test_html_prescan 20000)

# Our own tree-construction cases; they need no download.
file(GLOB HTML_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.dat)
add_test(NAME html_cases COMMAND test_html_tree ${HTML_CASES})

# The html5lib tokenizer tests and the WPT tree-construction tests, fetched
# by tests/html/fetch-suite.sh into build/. The runner script skips (exit
# 77) when they are absent.
add_test(NAME html5lib_tokenizer
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-suite.sh tokenizer $<TARGET_FILE:test_html_tokenizer>
                 ${CMAKE_SOURCE_DIR}/build ${CMAKE_CURRENT_SOURCE_DIR}/expected-failures.txt)
add_test(NAME html5lib_tree
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-suite.sh tree $<TARGET_FILE:test_html_tree>
                 ${CMAKE_SOURCE_DIR}/build ${CMAKE_CURRENT_SOURCE_DIR}/expected-failures.txt)
set_tests_properties(html5lib_tokenizer html5lib_tree PROPERTIES SKIP_RETURN_CODE 77)

# Tokenizer and tree builder fuzzing over the sample pages and the
# tree-construction cases (see fuzz_html.c). For a long run use the
# sanitizer build:
#   fuzz_html 100000000 SEED 600 tests/page/samples/*.html tests/html/cases/*.dat
add_executable(fuzz_html fuzz_html.c)
target_link_libraries(fuzz_html aura_engine)
file(GLOB HTML_FUZZ_PAGES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/page/samples/*.html)
add_test(NAME html_fuzz COMMAND fuzz_html 1500 1 20 ${HTML_FUZZ_PAGES} ${HTML_CASES})
set_tests_properties(html_fuzz PROPERTIES TIMEOUT 120)

# Inputs the fuzzer found, reduced: each must run to the end.
file(GLOB HTML_REGRESS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regress/*)
add_test(NAME html_fuzz_regress COMMAND fuzz_html replay ${HTML_REGRESS})

# In a plain build the fuzzer and its regression inputs run on a thread
# with a 512 KB stack (about what layout may use on Mac OS 9, where frames
# are smaller), so recursion over input nesting fails the test. Sanitizer
# frames are several times larger, so not there.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize")
    set_tests_properties(html_fuzz html_fuzz_regress PROPERTIES ENVIRONMENT "AURA_FUZZ_STACK_KB=512")
endif()
