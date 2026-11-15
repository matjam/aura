# JavaScript tests: each file runs in its own window through `aura-host js`
# after harness.js. A test fails on a "FAIL" line or an unexpected uncaught
# error (see src/platform/host/jsrun.c).
set(JS_TOOL $<TARGET_FILE:aura-host>)
set(JS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

foreach(name dom_tree events collections html_elements loop window net observers custom_elements shadow
             modules security)
    add_test(NAME js_${name} COMMAND ${JS_TOOL} js --quiet ${JS_DIR}/harness.js ${JS_DIR}/${name}.js)
endforeach()

add_test(NAME js_limits
         COMMAND ${JS_TOOL} js --quiet --mem 16 --kill 1000 --expect-errors 3 ${JS_DIR}/harness.js ${JS_DIR}/limits.js)
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
