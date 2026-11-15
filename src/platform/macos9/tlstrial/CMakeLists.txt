# TlsTrial: Mbed TLS 4.1 on Mac OS 9 (see tlstrial.c and
# docs/tls13-feasibility.md). A trial, not part of Aura: it is built only
# when both variables are set.
#   AURA_MBEDTLS_DIR  an unpacked Mbed TLS 4.1.x release
#   AURA_MBEDTLS_LIB  libmbedtrial.a from tests/net/mbedtls-trial/build.sh DIR OUT ppc
if(AURA_MBEDTLS_DIR AND AURA_MBEDTLS_LIB)
  enable_language(CXX)
  add_application(TlsTrial tlstrial.c ../net_ot.c ../entropy.c ../clock_mac.c CONSOLE CREATOR AUTT)
  target_include_directories(TlsTrial PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/net/mbedtls-trial
    ${AURA_MBEDTLS_DIR}/include
    ${AURA_MBEDTLS_DIR}/tf-psa-crypto/include
    ${AURA_MBEDTLS_DIR}/tf-psa-crypto/drivers/builtin/include)
  target_compile_definitions(TlsTrial PRIVATE
    MBEDTLS_CONFIG_FILE="aura_mbedtls_config.h"
    TF_PSA_CRYPTO_CONFIG_FILE="aura_crypto_config.h")
  target_compile_options(TlsTrial PRIVATE -O2)
  target_link_libraries(TlsTrial aura_engine ${AURA_MBEDTLS_LIB} OpenTransportLib OpenTptInternetLib m)
endif()
