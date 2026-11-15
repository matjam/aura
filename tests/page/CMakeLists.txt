add_executable(test_page test_page.c)
target_link_libraries(test_page aura_engine m)
target_compile_definitions(test_page PRIVATE AURA_PAGE_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_test(NAME page COMMAND test_page)

add_executable(fuzz_page fuzz_page.c)
target_link_libraries(fuzz_page aura_engine m)
add_test(NAME page_fuzz COMMAND fuzz_page 60 7)
set_tests_properties(page_fuzz PROPERTIES TIMEOUT 120)

# @font-face loading with random rules and loader answers.
add_executable(fuzz_webfont fuzz_webfont.c)
target_link_libraries(fuzz_webfont aura_engine m)
target_compile_definitions(fuzz_webfont PRIVATE AURA_PAGE_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
add_test(NAME page_webfont_fuzz COMMAND fuzz_webfont 3000 5 40)
set_tests_properties(page_webfont_fuzz PROPERTIES TIMEOUT 120)

# Smoke test: render each sample page to a PNG in the build folder (the
# host tool prints its timings). Inspect the PNGs by eye after layout work.
if(TARGET aura-host)
    file(GLOB PAGE_SAMPLES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.html)
    foreach(sample ${PAGE_SAMPLES})
        get_filename_component(name ${sample} NAME_WE)
        add_test(NAME page_sample_${name}
                 COMMAND aura-host render ${sample} -w 1000 -o ${CMAKE_CURRENT_BINARY_DIR}/sample-${name}.png)
    endforeach()
endif()

# Style, layout, paint, hit testing, scroll damage and text selection on
# mutated sample pages, script off (see fuzz_layout.c). For a long run use
# the sanitizer build:
#   fuzz_layout 100000000 SEED 600 tests/page/samples/*.html tests/page/samples/*.css tests/page/samples/*.gif
add_executable(fuzz_layout fuzz_layout.c)
target_link_libraries(fuzz_layout aura_engine m)
file(GLOB LAYOUT_FUZZ_CORPUS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.html
     ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.css ${CMAKE_CURRENT_SOURCE_DIR}/samples/*.gif)
add_test(NAME page_layout_fuzz COMMAND fuzz_layout 150 1 20 ${LAYOUT_FUZZ_CORPUS})
set_tests_properties(page_layout_fuzz PROPERTIES TIMEOUT 120)
file(GLOB LAYOUT_REGRESS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/regress/*.html)
add_test(NAME page_layout_fuzz_regress COMMAND fuzz_layout replay ${LAYOUT_REGRESS})

# In a plain build the fuzzer and its regression inputs run on a thread
# with a 512 KB stack (about what layout may use on Mac OS 9, where frames
# are smaller), so recursion over input nesting fails the test. Sanitizer
# frames are several times larger, so not there.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize")
    set_tests_properties(page_layout_fuzz page_layout_fuzz_regress PROPERTIES ENVIRONMENT "AURA_FUZZ_STACK_KB=512")
endif()
