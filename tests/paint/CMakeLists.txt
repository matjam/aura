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
