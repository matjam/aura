# The shared image fuzzer over every committed vector, included by
# tests/image/CMakeLists.txt. ctest runs a short session; for a long one
# build with sanitizers and run, from the repository root:
#   build/asan/tests/image/fuzz_image 1000000 1 tests/image/vectors/*/*
file(GLOB IMAGE_FUZZ_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vectors/*/*)
add_executable(fuzz_image ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_image.c)
target_link_libraries(fuzz_image aura_engine)
target_include_directories(fuzz_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_fuzz COMMAND fuzz_image 4000 1 ${IMAGE_FUZZ_VECTORS})
set_tests_properties(image_fuzz PROPERTIES TIMEOUT 300)
