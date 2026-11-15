# Image decoder tests. Each format has its own .cmake file (so decoders
# can be added independently), its committed vectors in vectors/<format>,
# and reference decodes made with the host's own decoders by the
# generator scripts (see each script's header).
#
# To run a long fuzz session, build with sanitizers (see CLAUDE.md) and run
#   fuzz_image ITERATIONS SEED tests/image/vectors/*/*
include(${CMAKE_CURRENT_SOURCE_DIR}/common.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/png.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/gif.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/bmp.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/jpeg.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/webp.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/cache.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/fuzz.cmake OPTIONAL)
