# Deflate encoder tests, included by tests/codec/CMakeLists.txt: round
# trips through Aura's inflate over the originals in brotli-vectors/ and
# two sample pages, a deterministic round-trip fuzzer, and (when Python is
# there) decoding with Python's zlib.
#
# To see sizes and speed:
#   build/host/tests/codec/test_deflate --bench tests/page/samples/wiki.html
# To run a long fuzz session, build fuzz_deflate with sanitizers and run
#   AURA_FUZZ_SECONDS=600 fuzz_deflate 100000000 SEED tests/codec/brotli-vectors/page.html
set(DEFLATE_ORIGINALS)
foreach(name craft-blocks craft-dict craft-edge craft-uncompressed empty one page.html random repeat samples text)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/brotli-vectors/${name})
        list(APPEND DEFLATE_ORIGINALS ${CMAKE_CURRENT_SOURCE_DIR}/brotli-vectors/${name})
    endif()
endforeach()
foreach(page wiki.html images.html)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/page/samples/${page})
        list(APPEND DEFLATE_ORIGINALS ${CMAKE_SOURCE_DIR}/tests/page/samples/${page})
    endif()
endforeach()

add_executable(test_deflate ${CMAKE_CURRENT_SOURCE_DIR}/test_deflate.c)
target_link_libraries(test_deflate aura_engine)
target_include_directories(test_deflate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME codec_deflate COMMAND test_deflate ${DEFLATE_ORIGINALS})

add_executable(fuzz_deflate ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_deflate.c)
target_link_libraries(fuzz_deflate aura_engine)
target_include_directories(fuzz_deflate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME codec_deflate_fuzz COMMAND fuzz_deflate 300 1 ${DEFLATE_ORIGINALS})
set_tests_properties(codec_deflate_fuzz PROPERTIES ENVIRONMENT "AURA_FUZZ_SECONDS=60")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME codec_deflate_zlib
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check-deflate.py $<TARGET_FILE:test_deflate>
                     ${DEFLATE_ORIGINALS})
endif()
