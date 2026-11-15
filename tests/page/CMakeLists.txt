add_executable(test_page test_page.c)
target_link_libraries(test_page aura_engine m)
add_test(NAME page COMMAND test_page)

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
