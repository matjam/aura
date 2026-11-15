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

# Parse and match timings on a generated sheet (and on files given as
# arguments). Registered with a small size so it also runs under ctest.
add_executable(css_bench css_bench.c)
target_link_libraries(css_bench aura_engine m)
add_test(NAME css_bench COMMAND css_bench --small)
