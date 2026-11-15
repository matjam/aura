add_executable(test_page test_page.c)
target_link_libraries(test_page aura_engine m)
add_test(NAME page COMMAND test_page)
