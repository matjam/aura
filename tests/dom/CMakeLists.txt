add_executable(test_dom test_dom.c)
target_link_libraries(test_dom aura_engine)
add_test(NAME dom COMMAND test_dom)
