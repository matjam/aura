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
set_tests_properties(browser_net_page PROPERTIES TIMEOUT 120)

# The document cache for Back and Forward, on the fake transport.
add_executable(test_back_cache test_back_cache.c)
target_link_libraries(test_back_cache aura_fakenet m)
target_include_directories(test_back_cache PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_back_cache COMMAND test_back_cache)
set_tests_properties(browser_back_cache PROPERTIES TIMEOUT 120)

# The files Aura keeps on disk (session, bookmarks, history, preferences,
# downloads, cookie rules, the disk cache folder), damaged (see
# fuzz_files.c). For a long run use the sanitizer build:
#   fuzz_files 100000000 SEED 600
add_executable(fuzz_files fuzz_files.c)
target_link_libraries(fuzz_files aura_engine m)
add_test(NAME browser_files_fuzz COMMAND fuzz_files 20000 1 20)
set_tests_properties(browser_files_fuzz PROPERTIES TIMEOUT 120)
