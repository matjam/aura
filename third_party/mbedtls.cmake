# Mbed TLS (TLS 1.2 and 1.3, X.509) with TF-PSA-Crypto, its cryptography
# library (third_party/mbedtls, see third_party/README.md). Included from
# the top-level CMakeLists.txt after the aura_engine library is defined.
# Builds the library sources as the static library aura_mbedtls with
# Aura's configuration (third_party/mbedtls-config) and links it into
# aura_engine. The release's own build files are not used.
#
# On the host the configuration adds the server side (AURA_MBEDTLS_SERVER)
# for the tests' in-process TLS server; the Mac OS 9 library is a client
# only. The definition is PUBLIC because the library's structures depend
# on it.
set(AURA_MBEDTLS_DIR ${CMAKE_CURRENT_LIST_DIR}/mbedtls)
set(AURA_TFPSA_DIR ${AURA_MBEDTLS_DIR}/tf-psa-crypto)
file(GLOB AURA_MBEDTLS_SOURCES CONFIGURE_DEPENDS
    ${AURA_MBEDTLS_DIR}/library/*.c
    ${AURA_TFPSA_DIR}/core/*.c
    ${AURA_TFPSA_DIR}/extras/*.c
    ${AURA_TFPSA_DIR}/platform/*.c
    ${AURA_TFPSA_DIR}/utilities/*.c
    ${AURA_TFPSA_DIR}/drivers/builtin/src/*.c)
# Files that Aura's configuration turns off compile to nothing, and Apple's
# archiver warns about each empty object, so they are left out. If a
# change to the configuration turns one of these on, the link fails with
# undefined symbols: take the file off this list.
set(AURA_MBEDTLS_UNUSED
    aesce aesni aria base64 bignum_mod_raw bignum_mod camellia ccm cipher_wrap cipher cmac ctr_drbg debug
    ecjpake ecp_curves_new error lmots lms md5 memory_buffer_alloc mps_trace net_sockets nist_kw pem pkcs5
    pkcs7 pkwrite psa_crypto_ffdh psa_crypto_pake psa_crypto_storage psa_crypto_xof psa_its_file ripemd160
    sha3 ssl_cookie ssl_debug_helpers_generated tf_psa_crypto_version threading timing version_features
    version x509_create x509_crl x509_csr x509write_crt x509write_csr x509write)
foreach(name ${AURA_MBEDTLS_UNUSED})
    list(FILTER AURA_MBEDTLS_SOURCES EXCLUDE REGEX "/${name}\\.c$")
endforeach()
# These two only check the configuration for consistency at compile time
# and define nothing: they are compiled, not archived.
list(FILTER AURA_MBEDTLS_SOURCES EXCLUDE REGEX "/(mbedtls_config|tf_psa_crypto_config)\\.c$")
add_library(aura_mbedtls STATIC ${AURA_MBEDTLS_SOURCES})
add_library(aura_mbedtls_check OBJECT
    ${AURA_MBEDTLS_DIR}/library/mbedtls_config.c
    ${AURA_TFPSA_DIR}/core/tf_psa_crypto_config.c)
target_include_directories(aura_mbedtls_check PRIVATE
    $<TARGET_PROPERTY:aura_mbedtls,INCLUDE_DIRECTORIES>)
target_compile_definitions(aura_mbedtls_check PRIVATE
    $<TARGET_PROPERTY:aura_mbedtls,COMPILE_DEFINITIONS>)
target_compile_options(aura_mbedtls_check PRIVATE -w)
add_dependencies(aura_mbedtls aura_mbedtls_check)
target_include_directories(aura_mbedtls SYSTEM PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/mbedtls-config
    ${AURA_MBEDTLS_DIR}/include
    ${AURA_TFPSA_DIR}/include
    ${AURA_TFPSA_DIR}/drivers/builtin/include)
target_include_directories(aura_mbedtls PRIVATE
    ${AURA_MBEDTLS_DIR}/library
    ${AURA_TFPSA_DIR}/core
    ${AURA_TFPSA_DIR}/dispatch
    ${AURA_TFPSA_DIR}/extras
    ${AURA_TFPSA_DIR}/platform
    ${AURA_TFPSA_DIR}/utilities
    ${AURA_TFPSA_DIR}/drivers/builtin/src)
target_compile_definitions(aura_mbedtls PUBLIC
    MBEDTLS_CONFIG_FILE="aura_mbedtls_config.h"
    TF_PSA_CRYPTO_CONFIG_FILE="aura_crypto_config.h")
if(NOT AURA_MACOS9)
    target_compile_definitions(aura_mbedtls PUBLIC AURA_MBEDTLS_SERVER)
endif()
# Third-party code: its warnings are not ours to fix. Always optimized;
# the public-key code is far too slow otherwise. The cryptography is
# built for speed; the protocol and X.509 code (library/), which runs a
# few times per connection, for size.
target_compile_options(aura_mbedtls PRIVATE -w -O2)
# Under the sanitizers: the library calls memcmp and memcpy with a null
# pointer and length 0 (x509_crt.c compares an empty OID that way), which
# the C standard forbids and every C library accepts. UBSan's check of
# nonnull arguments is turned off for this target only; our own code is
# still checked, and AddressSanitizer still checks every access here.
if(CMAKE_C_FLAGS MATCHES "-fsanitize")
    target_compile_options(aura_mbedtls PRIVATE -fno-sanitize=nonnull-attribute)
endif()
file(GLOB AURA_MBEDTLS_PROTOCOL_SOURCES CONFIGURE_DEPENDS ${AURA_MBEDTLS_DIR}/library/*.c)
set_source_files_properties(${AURA_MBEDTLS_PROTOCOL_SOURCES} PROPERTIES COMPILE_OPTIONS "-Os")
# The library calls three functions of the engine (the clocks and the
# entropy source in src/net/tls_platform.c). They are in the object file
# that every user of the library pulls in first (tls.c calls it), and
# the engine comes before the library on the link line, so a one-pass
# linker finds them.
target_link_libraries(aura_engine PUBLIC aura_mbedtls)
