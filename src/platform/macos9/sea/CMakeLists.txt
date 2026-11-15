# The Aura installer: the extractor that tools/make-sea.py joins with an
# archive of build/ppc/dist/Aura to make the self-extracting application
# (see sea.c). Builds AuraSEA.bin; ./build.sh ppc runs the packer.
#
# The 'vers' resource takes its version from AU_VERSION in
# src/browser/credits.h, which the About box also reads: "0.3" becomes
# 0x00, 0x30 (major, then minor and bug fix as two BCD digits).
file(STRINGS ${CMAKE_SOURCE_DIR}/src/browser/credits.h sea_line REGEX "^#define AU_VERSION ")
string(REGEX REPLACE "^#define AU_VERSION \"(.*)\".*$" "\\1" AURA_VERSION "${sea_line}")
if(NOT AURA_VERSION MATCHES "^([0-9]+)\\.([0-9])(\\.([0-9]))?")
    message(FATAL_ERROR "AU_VERSION \"${AURA_VERSION}\" is not MAJOR.MINOR or MAJOR.MINOR.FIX with one-digit MINOR and FIX")
endif()
set(SEA_VERS_MAJOR ${CMAKE_MATCH_1})
set(sea_fix ${CMAKE_MATCH_4})
if(sea_fix STREQUAL "")
    set(sea_fix 0)
endif()
set(SEA_VERS_MINOR ${CMAKE_MATCH_2}${sea_fix})
if(SEA_VERS_MAJOR LESS 10)
    set(SEA_VERS_MAJOR 0${SEA_VERS_MAJOR})
endif()
if(AURA_VERSION MATCHES "^0\\.")
    set(SEA_VERS_STAGE development)
else()
    set(SEA_VERS_STAGE release)
endif()
configure_file(sea_vers.r.in ${CMAKE_CURRENT_BINARY_DIR}/sea_vers.r @ONLY)

add_application(AuraSEA sea.c sea.r ../aura_icon.r ${CMAKE_CURRENT_BINARY_DIR}/sea_vers.r
                CREATOR AuSX)
target_include_directories(AuraSEA PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(AuraSEA PRIVATE -O2)
target_link_libraries(AuraSEA aura_engine)
