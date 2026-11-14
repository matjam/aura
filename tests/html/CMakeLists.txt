add_executable(test_html_tokenizer test_tokenizer.c json.c)
target_link_libraries(test_html_tokenizer aura_engine)

add_executable(test_html_tree test_tree.c)
target_link_libraries(test_html_tree aura_engine)

add_executable(test_html_api test_html_api.c)
target_link_libraries(test_html_api aura_engine)

# API, serializer, encoding, script and scaling tests.
add_test(NAME html_api COMMAND test_html_api)

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
