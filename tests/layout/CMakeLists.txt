add_executable(test_layout test_layout.c)
target_link_libraries(test_layout aura_engine m)

# Text selection, range text and find (src/layout/selection.c).
add_executable(test_selection test_selection.c)
target_link_libraries(test_selection aura_engine m)
add_test(NAME layout_selection COMMAND test_selection)

# Mixed scripts, symbols and emoji with the bundled fonts.
add_executable(test_layout_fonts test_fonts.c)
target_link_libraries(test_layout_fonts aura_host_platform aura_engine m)
target_compile_definitions(test_layout_fonts PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
add_test(NAME layout_fonts COMMAND test_layout_fonts)

# One test per case file.
file(GLOB LAYOUT_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.txt)
foreach(case ${LAYOUT_CASES})
    get_filename_component(name ${case} NAME_WE)
    add_test(NAME layout_${name} COMMAND test_layout ${case})
    # Some cases guard against exponential layout time: fail, do not hang.
    set_tests_properties(layout_${name} PROPERTIES TIMEOUT 60)
endforeach()
