# Brotli decoder tests, included by tests/codec/CMakeLists.txt.
#
# To run a long fuzz session, build fuzz_brotli with sanitizers and run
#   fuzz_brotli ITERATIONS SEED tests/codec/brotli-vectors/*.br
file(GLOB BROTLI_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/brotli-vectors/*.br)

add_executable(test_brotli ${CMAKE_CURRENT_SOURCE_DIR}/test_brotli.c)
target_link_libraries(test_brotli aura_engine)
target_include_directories(test_brotli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME codec_brotli COMMAND test_brotli ${BROTLI_VECTORS})

add_executable(fuzz_brotli ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_brotli.c)
target_link_libraries(fuzz_brotli aura_engine)
target_include_directories(fuzz_brotli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME codec_brotli_fuzz COMMAND fuzz_brotli 3000 1 ${BROTLI_VECTORS})
