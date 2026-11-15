add_executable(test_layout test_layout.c)
target_link_libraries(test_layout aura_engine m)

# One test per case file.
file(GLOB LAYOUT_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cases/*.txt)
foreach(case ${LAYOUT_CASES})
    get_filename_component(name ${case} NAME_WE)
    add_test(NAME layout_${name} COMMAND test_layout ${case})
    # Some cases guard against exponential layout time: fail, do not hang.
    set_tests_properties(layout_${name} PROPERTIES TIMEOUT 60)
endforeach()
