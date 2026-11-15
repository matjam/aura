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
