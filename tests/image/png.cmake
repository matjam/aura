# PNG tests, included by tests/image/CMakeLists.txt. The vectors and
# their references are made by gen-png-vectors.py and committed. PngSuite
# is fetched into build/pngsuite by fetch-pngsuite.sh (not committed);
# image_png_suite is skipped when it is absent.
add_executable(test_png ${CMAKE_CURRENT_SOURCE_DIR}/test_png.c)
target_link_libraries(test_png aura_engine)
target_include_directories(test_png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_png COMMAND test_png ${CMAKE_CURRENT_SOURCE_DIR}/vectors/png)
add_test(NAME image_png_suite COMMAND test_png ${CMAKE_CURRENT_SOURCE_DIR}/vectors/png ${CMAKE_SOURCE_DIR}/build/pngsuite)
set_tests_properties(image_png_suite PROPERTIES SKIP_RETURN_CODE 77)
