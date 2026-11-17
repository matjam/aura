# Linux packaging for the GTK4 port (docs/gtk.md, "Packaging"): the install
# rules, and the CPack settings for the .deb. tools/package-linux.sh builds
# the .deb and the AppImage from these rules. GTK4 is linked from the
# system and is never installed here.
include(GNUInstallDirs)

# The version, from the line the About box reads (docs/ci-and-releases.md).
file(STRINGS ${CMAKE_SOURCE_DIR}/src/browser/credits.h AURA_VERSION_LINE
     REGEX "^#define AU_VERSION \"")
string(REGEX REPLACE "^#define AU_VERSION \"([^\"]*)\".*$" "\\1"
       AURA_VERSION "${AURA_VERSION_LINE}")
if(NOT AURA_VERSION)
    message(FATAL_ERROR "AU_VERSION not found in src/browser/credits.h")
endif()
string(TIMESTAMP AURA_RELEASE_DATE "%Y-%m-%d" UTC)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/org.aura.Browser.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.aura.Browser.metainfo.xml @ONLY)

# The binary finds its assets in ../share/aura beside itself (files_gtk.c).
install(TARGETS aura RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/fonts ${CMAKE_SOURCE_DIR}/assets/net
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aura)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/org.aura.Browser.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.aura.Browser.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME org.aura.Browser.svg)
install(FILES ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME org.aura.Browser.png)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/aura)

# The .deb: depends on the distribution's GTK4 and fontconfig libraries.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME aura)
set(CPACK_PACKAGE_VERSION ${AURA_VERSION})
set(CPACK_PACKAGE_CONTACT "Nathan Ollerenshaw <578676+matjam@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight web browser with its own engine")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/matjam/aura")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_SECTION web)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-4-1 (>= 4.12), libfontconfig1, libc6")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
