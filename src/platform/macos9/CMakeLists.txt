# Aura for Mac OS 9 (Retro68, PowerPC). Builds Aura.bin (MacBinary, for
# tools/guest-push.sh), Aura.dsk (an HFS disk image) and Aura.APPL.
add_application(Aura main.c platform_mac.c aura.r CREATOR AURA)
target_include_directories(Aura PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(Aura PRIVATE -O2)
target_link_libraries(Aura aura_engine m)
