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

    # Scrolling by moving pixels matches a fresh paint.
    add_executable(test_scroll_shift test_scroll_shift.c)
    target_link_libraries(test_scroll_shift aura_host_platform aura_host_png aura_engine m)
    target_compile_definitions(test_scroll_shift PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts"
                               AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME scroll_shift COMMAND test_scroll_shift)
    set_tests_properties(scroll_shift PROPERTIES TIMEOUT 300)
endif()

# The fetcher path on src/net's fake transport (no network).
add_executable(test_net_page test_net_page.c ${CMAKE_SOURCE_DIR}/tests/net/tls_fixtures.c)
target_link_libraries(test_net_page aura_fakenet m)
target_include_directories(test_net_page PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_net_page COMMAND test_net_page)

# The developer tools' models: request log, time counters, console, page
# source and the text view, on the fake transport.
add_executable(test_devtools test_devtools.c ${CMAKE_SOURCE_DIR}/tests/net/tls_fixtures.c)
target_link_libraries(test_devtools aura_fakenet m)
target_include_directories(test_devtools PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_devtools COMMAND test_devtools)
set_tests_properties(browser_devtools PROPERTIES TIMEOUT 120)
set_tests_properties(browser_net_page PROPERTIES TIMEOUT 120)

# The document cache for Back and Forward, on the fake transport.
add_executable(test_back_cache test_back_cache.c)
target_link_libraries(test_back_cache aura_fakenet m)
target_include_directories(test_back_cache PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_back_cache COMMAND test_back_cache)
set_tests_properties(browser_back_cache PROPERTIES TIMEOUT 120)

# crypto.getRandomValues and randomUUID from AuBrowserHost.random.
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto aura_engine m)
add_test(NAME browser_crypto COMMAND test_crypto)

# Site icons for tabs, on the fake transport.
add_executable(test_favicon test_favicon.c)
target_link_libraries(test_favicon aura_fakenet m)
target_include_directories(test_favicon PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
target_compile_definitions(test_favicon PRIVATE AURA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME browser_favicon COMMAND test_favicon)
set_tests_properties(browser_favicon PROPERTIES TIMEOUT 120)

# The bug report and the System Status report: structure, ASCII, and what
# is left out (cookies, Authorization, long queries, paths, other tabs).
add_executable(test_bugreport test_bugreport.c)
target_link_libraries(test_bugreport aura_fakenet m)
target_include_directories(test_bugreport PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_bugreport COMMAND test_bugreport)
set_tests_properties(browser_bugreport PROPERTIES TIMEOUT 120)
