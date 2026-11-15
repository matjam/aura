# Decompression tests: inflate (zlib, gzip, raw) and brotli, against
# vectors generated on the host, corrupt input, and a deterministic fuzzer.
include(${CMAKE_CURRENT_SOURCE_DIR}/inflate.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/brotli.cmake OPTIONAL)
