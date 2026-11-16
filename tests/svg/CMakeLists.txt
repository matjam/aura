# SVG tests: grammars, XML, rendering, and fuzzers for the XML parser,
# the path parser and the SVG image pipeline. ctest runs short fuzz
# sessions; for long ones build with sanitizers (see docs/building.md) and run
#   build/asan/tests/svg/fuzz_svg ITERATIONS SEED tests/svg/corpus/*
add_executable(test_svg ${CMAKE_CURRENT_SOURCE_DIR}/test_svg.c)
target_link_libraries(test_svg aura_engine)
target_include_directories(test_svg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME svg COMMAND test_svg ${CMAKE_SOURCE_DIR}/assets/fonts/liberation/AuraSans-Regular.ttf)

file(GLOB SVG_FUZZ_CORPUS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/*)
add_executable(fuzz_svg ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_svg.c)
target_link_libraries(fuzz_svg aura_engine)
target_include_directories(fuzz_svg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME svg_fuzz COMMAND fuzz_svg 20000 1 ${SVG_FUZZ_CORPUS})
set_tests_properties(svg_fuzz PROPERTIES TIMEOUT 300)
