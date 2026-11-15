add_executable(test_browser test_browser.c)
target_link_libraries(test_browser aura_engine m)
add_test(NAME browser COMMAND test_browser)

add_executable(test_page_doc test_page_doc.c)
target_link_libraries(test_page_doc aura_engine m)
target_compile_definitions(test_page_doc PRIVATE AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME page_doc COMMAND test_page_doc)
