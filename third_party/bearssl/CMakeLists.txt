# BearSSL 0.6 as a static library (see third_party/README.md).
#
# Every source file is compiled; the linker keeps only the objects Aura
# references (src/net/tls.c picks the 32-bit implementations: i31/m31 big
# integers, aes_ct, ghash_ctmul32, chacha20_ct, poly1305_ctmul), so the
# 64-bit and SIMD variants cost nothing in the application. They compile
# to empty objects where their CPU feature is absent.
#
# BearSSL's own OS hooks are disabled: Aura supplies the time for
# certificate validation and the seed for the DRBG itself, so the library
# makes no system calls (no /dev/urandom, no time()).
file(GLOB_RECURSE BEARSSL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
# CPU-specific variants (x86 AES-NI/SSE2/PCLMUL, POWER8) are left out, and
# turned off below (BR_AES_X86NI, BR_SSE2, BR_POWER8) so that no other file
# refers to them on an x86 host, where BearSSL would turn them on.
list(FILTER BEARSSL_SOURCES EXCLUDE REGEX "_(x86ni|pwr8|sse2|pclmul)\\.c$")
add_library(bearssl STATIC ${BEARSSL_SOURCES})
target_include_directories(bearssl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(bearssl PRIVATE BR_USE_URANDOM=0 BR_USE_WIN32_RAND=0 BR_USE_UNIX_TIME=0
                                           BR_USE_WIN32_TIME=0 BR_RDRAND=0 BR_POWER8=0
                                           BR_AES_X86NI=0 BR_SSE2=0)
# Third-party code keeps its own style; do not apply Aura's warning flags.
target_compile_options(bearssl PRIVATE -w -O2)
# Under the sanitizers (a host build with -fsanitize in CMAKE_C_FLAGS),
# leave out two checks BearSSL fails by design: it reads unaligned words
# on x86, and calls hash functions through pointers of a compatible but
# different type (Clang's function check).
if(CMAKE_C_FLAGS MATCHES "-fsanitize")
    target_compile_options(bearssl PRIVATE -fno-sanitize=alignment)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(bearssl PRIVATE -fno-sanitize=function)
    endif()
endif()
