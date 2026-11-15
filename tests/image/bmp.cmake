# BMP, ICO and favicon tests, included by tests/image/CMakeLists.txt. The
# vectors and their references are made by gen-bmp-vectors.py and
# committed.
add_executable(test_bmp ${CMAKE_CURRENT_SOURCE_DIR}/test_bmp.c)
target_link_libraries(test_bmp aura_engine)
target_include_directories(test_bmp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_bmp COMMAND test_bmp ${CMAKE_CURRENT_SOURCE_DIR}/vectors/bmp)
