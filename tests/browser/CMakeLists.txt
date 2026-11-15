add_executable(test_browser test_browser.c)
target_link_libraries(test_browser aura_engine m)
add_test(NAME browser COMMAND test_browser)

add_executable(test_page_doc test_page_doc.c)
target_link_libraries(test_page_doc aura_engine m)
target_compile_definitions(test_page_doc PRIVATE AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME page_doc COMMAND test_page_doc)

# Chrome text through the font fallback API, with the bundled fonts.
if(TARGET aura_host_platform)
    add_executable(test_chrome_fonts test_chrome_fonts.c)
    target_link_libraries(test_chrome_fonts aura_host_platform aura_engine m)
    target_compile_definitions(test_chrome_fonts PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
    add_test(NAME chrome_fonts COMMAND test_chrome_fonts)
endif()

# The fetcher path on src/net's fake transport (no network).
add_executable(test_net_page test_net_page.c ${CMAKE_SOURCE_DIR}/tests/net/tls_fixtures.c)
target_link_libraries(test_net_page aura_fakenet m)
target_include_directories(test_net_page PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_net_page COMMAND test_net_page)
set_tests_properties(browser_net_page PROPERTIES TIMEOUT 120)
