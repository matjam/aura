add_executable(test_paint test_paint.c test_font.c)
target_link_libraries(test_paint aura_host_png aura_engine)
target_compile_definitions(test_paint PRIVATE
    AURA_PAINT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME paint COMMAND test_paint)

# Bundled fonts, aliases and fallback, on assets/fonts.
add_executable(test_fallback test_fallback.c)
target_link_libraries(test_fallback aura_host_platform aura_engine)
target_compile_definitions(test_fallback PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
add_test(NAME paint_fallback COMMAND test_fallback)

# Variable fonts against fontTools instances: committed subsets, and the
# full fonts when tools/test-fonts.py has fetched them (skipped otherwise).
add_executable(test_fontvar test_fontvar.c)
target_link_libraries(test_fontvar aura_host_platform aura_engine m)
add_test(NAME paint_fontvar COMMAND test_fontvar ${CMAKE_CURRENT_SOURCE_DIR}/fonts)
add_test(NAME paint_fontvar_full COMMAND test_fontvar ${CMAKE_SOURCE_DIR}/build/test-fonts/refs --optional)
set_tests_properties(paint_fontvar_full PROPERTIES SKIP_RETURN_CODE 77)

# Font parser fuzzing (run long under the sanitizer build:
# fuzz_font 1000000 SEED 3600 tests/paint/fonts/*.ttf).
add_executable(fuzz_font fuzz_font.c)
target_link_libraries(fuzz_font aura_host_platform aura_engine m)
file(GLOB FUZZ_FONT_SEEDS ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.ttf ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.woff*)
add_test(NAME paint_font_fuzz COMMAND fuzz_font 20000 11 40 ${FUZZ_FONT_SEEDS})
set_tests_properties(paint_font_fuzz PROPERTIES TIMEOUT 120)
