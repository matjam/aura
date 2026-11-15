# WebP decoder tests, included by tests/image/CMakeLists.txt. The vectors
# in vectors/webp are made by gen-webp-vectors.py (cwebp, webpmux,
# img2webp and dwebp) and committed.
#
# To run a long fuzz session, build with sanitizers and run
#   test_webp --fuzz ITERATIONS SEED tests/image/vectors/webp/*.webp
# To measure decode speed, make the large images and run
#   python3 tests/image/gen-webp-vectors.py --big build/webp
#   test_webp --bench build/webp/big_lossy.webp
#   test_webp --bench build/webp/big_lossless.webp
# To check against libwebp's test data (not committed), clone it and make
# its references, then run ctest (image_webp_testdata is skipped without
# it):
#   git clone https://chromium.googlesource.com/webm/libwebp-test-data build/webp-testdata
#   python3 tests/image/gen-webp-vectors.py --testdata build/webp-testdata
file(GLOB WEBP_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vectors/webp/*.webp)

add_executable(test_webp ${CMAKE_CURRENT_SOURCE_DIR}/test_webp.c)
target_link_libraries(test_webp aura_engine)
target_include_directories(test_webp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_webp COMMAND test_webp ${CMAKE_CURRENT_SOURCE_DIR}/vectors/webp)
add_test(NAME image_webp_fuzz COMMAND test_webp --fuzz 3000 1 ${WEBP_VECTORS})
add_test(NAME image_webp_testdata COMMAND test_webp --testdata ${CMAKE_SOURCE_DIR}/build/webp-testdata)
set_tests_properties(image_webp_testdata PROPERTIES SKIP_RETURN_CODE 77)
