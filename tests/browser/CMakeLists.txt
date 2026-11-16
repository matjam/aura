add_executable(test_browser test_browser.c)
target_link_libraries(test_browser aura_engine m)
add_test(NAME browser COMMAND test_browser)

# The command table: titles, shortcuts, enable rules, running commands,
# the port's callbacks and the dynamic menus.
add_executable(test_commands test_commands.c)
target_link_libraries(test_commands aura_engine m)
add_test(NAME browser_commands COMMAND test_commands)

# The application services over an in-memory port: the files Aura
# keeps, damaged files, when each is saved, the Running marker and
# session restore, and the network stack set up from the preferences.
add_executable(test_app test_app.c)
target_link_libraries(test_app aura_engine m)
add_test(NAME browser_app COMMAND test_app)

# The list windows' models: rows, actions and their enable rules, the
# callbacks for actions that ask, and the change notice.
add_executable(test_lists test_lists.c)
target_link_libraries(test_lists aura_engine m)
add_test(NAME browser_lists COMMAND test_lists)

# Native chrome mode: the page-only surface, the actions and accessors a
# port's own widgets use, and the AU_CHANGED_* notices.
add_executable(test_native_chrome test_native_chrome.c)
target_link_libraries(test_native_chrome aura_engine m)
add_test(NAME browser_native_chrome COMMAND test_native_chrome)

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

# Unloaded tabs, on the fake transport.
add_executable(test_unload test_unload.c)
target_link_libraries(test_unload aura_fakenet m)
target_include_directories(test_unload PRIVATE ${CMAKE_SOURCE_DIR}/tests/net)
add_test(NAME browser_unload COMMAND test_unload)
set_tests_properties(browser_unload PROPERTIES TIMEOUT 120)

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

# Script memory: a page, two frames and a worker that share one large
# script stay under stated peaks; out-of-memory in a worker or a page is
# reported with the runtime, its limit and the preference to raise.
add_executable(test_script_memory test_script_memory.c)
target_link_libraries(test_script_memory aura_fakenet m)
target_include_directories(test_script_memory PRIVATE ${CMAKE_SOURCE_DIR}/tests/net ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME browser_script_memory COMMAND test_script_memory)
set_tests_properties(browser_script_memory PROPERTIES TIMEOUT 300)

# The files Aura keeps on disk (session, bookmarks, history, preferences,
# downloads, cookie rules, the disk cache folder), damaged (see
# fuzz_files.c). For a long run use the sanitizer build:
#   fuzz_files 100000000 SEED 600
add_executable(fuzz_files fuzz_files.c)
target_link_libraries(fuzz_files aura_engine m)
add_test(NAME browser_files_fuzz COMMAND fuzz_files 20000 1 20)
set_tests_properties(browser_files_fuzz PROPERTIES TIMEOUT 120)
