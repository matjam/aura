# JavaScript tests: each file runs in its own window through `aura-host js`
# after harness.js. A test fails on a "FAIL" line or an unexpected uncaught
# error (see src/platform/host/jsrun.c).
set(JS_TOOL $<TARGET_FILE:aura-host>)
set(JS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

foreach(name dom_tree events collections html_elements loop window net observers custom_elements shadow
             modules security forms ranges cssom)
    add_test(NAME js_${name} COMMAND ${JS_TOOL} js --quiet ${JS_DIR}/harness.js ${JS_DIR}/${name}.js)
endforeach()

# Workers on their own stacks (the host's coroutines), and without them.
add_test(NAME js_workers
         COMMAND ${JS_TOOL} js --quiet --expect-errors 1 ${JS_DIR}/harness.js ${JS_DIR}/workers.js)
add_test(NAME js_workers_inline
         COMMAND ${JS_TOOL} js --quiet --no-coro --kill 300 --expect-errors 1 ${JS_DIR}/harness.js ${JS_DIR}/workers_inline.js)

# The kill timer is 4 s: the scripts that must finish (deep trees, filling
# the heap) take well under 100 ms on an idle machine, and with 1 s a busy
# machine or a sanitizer build stopped one of them as running too long.
# The two endless loops then take 4 s each.
add_test(NAME js_limits
         COMMAND ${JS_TOOL} js --quiet --mem 16 --kill 4000 --expect-errors 5 ${JS_DIR}/harness.js ${JS_DIR}/limits.js)
set_tests_properties(js_limits PROPERTIES TIMEOUT 300)
# Date with the embedder's clock and zone (au_js_set_clock): a fixed time,
# 2023-11-14T22:13:20.000Z, in a zone 8 hours west of UTC.
add_test(NAME js_clock
         COMMAND ${JS_TOOL} js --quiet --clock 1700000000000 --zone -28800 ${JS_DIR}/harness.js ${JS_DIR}/clock.js)
add_test(NAME js_lifetime
         COMMAND ${JS_TOOL} js --quiet ${JS_DIR}/harness.js ${JS_DIR}/lifetime_setup.js ${JS_DIR}/lifetime_check.js)

# Pages with scripts through src/page (parser-blocking scripts,
# document.write, defer/async, lifecycle, input), each fed in 256-byte
# chunks and one byte at a time. A "<!-- page args: ... -->" comment adds
# options (clicks).
file(GLOB JS_PAGES CONFIGURE_DEPENDS ${JS_DIR}/pages/*.html)
foreach(page ${JS_PAGES})
    get_filename_component(stem ${page} NAME_WE)
    file(READ ${page} body)
    set(args)
    if(body MATCHES "<!-- page args: ([^\n]*) -->")
        separate_arguments(args UNIX_COMMAND "${CMAKE_MATCH_1}")
    endif()
    add_test(NAME js_page_${stem} COMMAND ${JS_TOOL} page --quiet ${args} ${page})
    add_test(NAME js_page_${stem}_bytewise COMMAND ${JS_TOOL} page --quiet --chunk 1 ${args} ${page})
endforeach()

# QuickJS-ng's own tests that need no qjs:std, qjs:os or worker support
# (third_party/quickjs/tests), each as a module in a window. Fixtures are
# imported by other tests, not run.
# A file with import or export statements runs as a module; a test262
# "negative" header names the error the test must throw.
# Not applicable: import-attributes-identity imports a file as type "text"
# (a qjs extension; Aura's loader does not implement import attribute
# module types).
set(QJS_NOT_APPLICABLE import-attributes-identity)
file(GLOB QJS_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/quickjs/tests/*.js)
foreach(t ${QJS_TESTS})
    get_filename_component(stem ${t} NAME_WE)
    if(stem MATCHES "^fixture_" OR stem STREQUAL "assert" OR stem IN_LIST QJS_NOT_APPLICABLE)
        continue()
    endif()
    file(READ ${t} body)
    set(mode)
    if(body MATCHES "(^|\n)[ \t]*(import[ \t{*\"']|export[ \t{])")
        set(mode --module)
    endif()
    set(neg)
    if(body MATCHES "negative:[^\n]*\n[^\n]*phase:[^\n]*\n[ \t]*type:[ \t]*([A-Za-z]+)")
        set(neg --negative ${CMAKE_MATCH_1})
    endif()
    add_test(NAME js_engine_${stem} COMMAND ${JS_TOOL} js --quiet ${mode} ${neg} ${JS_DIR}/engine_setup.js ${t})
endforeach()

# The DOM bindings under generated scripts, with layout, paint and input
# between steps (see fuzz_dom.c). For a long run use the sanitizer build:
#   fuzz_dom 100000000 SEED 600
add_executable(fuzz_dom fuzz_dom.c)
target_link_libraries(fuzz_dom aura_engine m)
add_test(NAME js_dom_fuzz COMMAND fuzz_dom 60 1 20)
set_tests_properties(js_dom_fuzz PROPERTIES TIMEOUT 120)
file(GLOB DOM_REGRESS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regress/*.js)
# A script that never ends must be stopped by its time limit: one that
# is not shows as a timeout here.
add_test(NAME js_dom_fuzz_regress COMMAND fuzz_dom replay ${DOM_REGRESS})
set_tests_properties(js_dom_fuzz_regress PROPERTIES TIMEOUT 300)
