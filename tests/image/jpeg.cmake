# JPEG decoder tests, included by tests/image/CMakeLists.txt. The vectors
# in vectors/jpeg are made by gen-jpeg-vectors.py (cjpeg, djpeg and
# ImageMagick) and committed.
#
# To run a long fuzz session, build with sanitizers and run
#   test_jpeg --fuzz ITERATIONS SEED tests/image/vectors/jpeg/*.jpg
# To measure decode speed, make the large images and run
#   python3 tests/image/gen-jpeg-vectors.py --big build/jpeg
#   test_jpeg --bench build/jpeg/big420.jpg
file(GLOB JPEG_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vectors/jpeg/*.jpg)

add_executable(test_jpeg ${CMAKE_CURRENT_SOURCE_DIR}/test_jpeg.c)
target_link_libraries(test_jpeg aura_engine)
target_include_directories(test_jpeg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_jpeg COMMAND test_jpeg ${CMAKE_CURRENT_SOURCE_DIR}/vectors/jpeg)
add_test(NAME image_jpeg_fuzz COMMAND test_jpeg --fuzz 10000 1 ${JPEG_VECTORS})
