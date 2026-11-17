# aura: the Linux GTK4 application, native Wayland (docs/gtk.md). Its OS
# services are src/platform/posix (aura_posix); everything that includes a
# GTK, GLib, Cairo or fontconfig header is in this folder.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AURA_GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(AURA_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
include(GNUInstallDirs)

# The native windows (Preferences, the lists, the developer windows, About)
# come from windows.cmake, which sets AURA_GTK_WINDOW_SOURCES. Without it
# the port builds alone and menus.c reports those windows as not available.
set(AURA_GTK_WINDOW_SOURCES "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake)
endif()

add_executable(aura
    main.c
    window.c
    input.c
    menus.c
    host.c
    net_gtk.c
    files_gtk.c
    fonts_gtk.c
    downloads_gtk.c
    sysinfo_gtk.c
    ${AURA_GTK_WINDOW_SOURCES})
target_link_libraries(aura aura_posix aura_engine PkgConfig::AURA_GTK4 PkgConfig::AURA_FONTCONFIG m)
target_include_directories(aura PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aura PRIVATE
    # The asset folders tried after $AURA_ASSETS and the folders beside
    # the binary (files_gtk.c, docs/gtk.md "Assets").
    AURA_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets"
    AURA_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/aura"
    # The shell's blocks live as long as the program (src/core/core.h, "Zones").
    AU_HEAP_ZONE=1)
