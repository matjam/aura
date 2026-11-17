# Aura for Mac OS 9 (Retro68, PowerPC). Builds Aura.bin (MacBinary, for
# tools/guest-push.sh), Aura.dsk (an HFS disk image) and Aura.APPL.
# AppearanceLib supplies the Appearance Manager (Preferences window).
# Open Transport (net_ot.c) comes from OpenTransportLib and
# OpenTptInternetLib.
# The 'vers' resources of Aura and of the installer come from AU_VERSION in
# src/browser/credits.h, which the About box also reads: "0.3.0" becomes
# 0x00, 0x30 (major, then minor and bug fix as two BCD digits). The .r
# files are generated, so a version change rebuilds them.
file(STRINGS ${CMAKE_SOURCE_DIR}/src/browser/credits.h aura_version_line REGEX "^#define AU_VERSION ")
string(REGEX REPLACE "^#define AU_VERSION \"([^\"]*)\".*$" "\\1" AURA_VERSION "${aura_version_line}")
if(NOT AURA_VERSION MATCHES "^([0-9]+)\\.([0-9])\\.([0-9])$")
    message(FATAL_ERROR "AU_VERSION \"${AURA_VERSION}\" is not MAJOR.MINOR.FIX with one-digit MINOR and FIX")
endif()
set(AURA_VERS_MAJOR ${CMAKE_MATCH_1})
set(AURA_VERS_MINOR ${CMAKE_MATCH_2}${CMAKE_MATCH_3})
if(AURA_VERS_MAJOR LESS 10)
    set(AURA_VERS_MAJOR 0${AURA_VERS_MAJOR})
endif()
if(AURA_VERSION MATCHES "^0\\.")
    set(AURA_VERS_STAGE development)
else()
    set(AURA_VERS_STAGE release)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/browser/credits.h)
configure_file(aura_vers.r.in ${CMAKE_CURRENT_BINARY_DIR}/aura_vers.r @ONLY)

# memcpy and memmove in place of newlib's (fastmem.c). The flags keep the
# compiler from turning its loops back into calls to memcpy.
add_library(aura_fastmem OBJECT fastmem.c)
target_compile_options(aura_fastmem PRIVATE -O2 -fno-builtin -fno-tree-loop-distribute-patterns)

add_application(Aura main.c browserwin.c listwin.c devwin.c sysinfo_mac.c prefswin.c storage.c text.c aboutbox.c contextmenu.c internetconfig.c coro_mac.c platform_mac.c clock_mac.c
                network.c downloads_mac.c net_ot.c entropy.c blocking_mac.c prefs_blocking.c aura.r aura_icon.r
                ${CMAKE_CURRENT_BINARY_DIR}/aura_vers.r
                CREATOR AURA)
target_sources(Aura PRIVATE $<TARGET_OBJECTS:aura_fastmem>)
target_include_directories(Aura PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(Aura PRIVATE -O2)
# The shell's blocks live as long as the application (src/core/core.h, "Zones").
target_compile_definitions(Aura PRIVATE AU_HEAP_ZONE=1)
target_link_libraries(Aura aura_engine AppearanceLib OpenTransportLib OpenTptInternetLib ThreadsLib m)
add_subdirectory(nettest)
add_subdirectory(jstest)
add_subdirectory(sea)
