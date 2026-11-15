add_executable(test_css test_css.c)
target_link_libraries(test_css aura_engine m)

# One test per case file.
file(GLOB CSS_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.txt)
foreach(case ${CSS_CASES})
    get_filename_component(name ${case} NAME_WE)
    add_test(NAME css_${name} COMMAND test_css ${case})
endforeach()

add_executable(test_css_api test_css_api.c)
target_link_libraries(test_css_api aura_engine m)
add_test(NAME css_api COMMAND test_css_api)

# Targeted invalidation of state changes against full restyles; --bench
# PAGE.html [SHEET.css ...] times hover moves over a page.
add_executable(test_css_invalidate test_css_invalidate.c)
target_link_libraries(test_css_invalidate aura_engine m)
add_test(NAME css_invalidate COMMAND test_css_invalidate)

# Parse and match timings on a generated sheet (and on files given as
# arguments). Registered with a small size so it also runs under ctest.
add_executable(css_bench css_bench.c)
target_link_libraries(css_bench aura_engine m)
add_test(NAME css_bench COMMAND css_bench --small)

# Tokenizer, parser, cascade, layout and paint on hostile sheets (see
# fuzz_css.c). For a long run use the sanitizer build:
#   fuzz_css 100000000 SEED 600 tests/page/samples/*.css tests/css/cases/*.txt
add_executable(fuzz_css fuzz_css.c)
target_link_libraries(fuzz_css aura_engine m)
add_test(NAME css_fuzz COMMAND fuzz_css 300 1 20 ${CMAKE_SOURCE_DIR}/tests/page/samples/wiki.css ${CSS_CASES})
set_tests_properties(css_fuzz PROPERTIES TIMEOUT 120)

# Inputs the fuzzer found, reduced: each must run to the end.
file(GLOB CSS_REGRESS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regress/*)
add_test(NAME css_fuzz_regress COMMAND fuzz_css replay ${CSS_REGRESS})

# In a plain build the fuzzer and its regression inputs run on a thread
# with a 512 KB stack (about what layout may use on Mac OS 9, where frames
# are smaller), so recursion over input nesting fails the test. Sanitizer
# frames are several times larger, so not there.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize")
    set_tests_properties(css_fuzz css_fuzz_regress PROPERTIES ENVIRONMENT "AURA_FUZZ_STACK_KB=512")
endif()
