# Aura for Mac OS 9 (Retro68, PowerPC). Builds Aura.bin (MacBinary, for
# tools/guest-push.sh), Aura.dsk (an HFS disk image) and Aura.APPL.
# AppearanceLib supplies the Appearance Manager (Preferences window).
# Open Transport (net_ot.c) comes from OpenTransportLib and
# OpenTptInternetLib.
add_application(Aura main.c browserwin.c listwin.c prefswin.c storage.c text.c aboutbox.c contextmenu.c internetconfig.c platform_mac.c clock_mac.c
                network.c downloads_mac.c net_ot.c entropy.c aura.r aura_icon.r
                CREATOR AURA)
target_include_directories(Aura PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(Aura PRIVATE -O2)
target_link_libraries(Aura aura_engine AppearanceLib OpenTransportLib OpenTptInternetLib m)
add_subdirectory(nettest)
