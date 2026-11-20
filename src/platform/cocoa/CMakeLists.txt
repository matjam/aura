# Aura.app: the modern macOS application, AppKit (docs/cocoa.md). Its OS
# services are src/platform/posix (aura_posix); everything that includes an
# AppKit, Foundation, Core Graphics or Core Text header is in this folder.
# The sources are Objective-C with ARC.

# The native windows (Preferences, the lists, the developer windows, About)
# come from windows.cmake, which sets AURA_COCOA_WINDOW_SOURCES.
include(${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake)

# The version, from the line the About box reads (docs/ci-and-releases.md).
file(STRINGS ${CMAKE_SOURCE_DIR}/src/browser/credits.h AURA_VERSION_LINE
     REGEX "^#define AU_VERSION \"")
string(REGEX REPLACE "^#define AU_VERSION \"([^\"]*)\".*$" "\\1"
       AURA_VERSION "${AURA_VERSION_LINE}")
configure_file(${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
               ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

# The application icon, made from the kookaburra PNG with sips and iconutil.
set(AURA_ICONSET ${CMAKE_CURRENT_BINARY_DIR}/Aura.iconset)
set(AURA_ICNS ${CMAKE_CURRENT_BINARY_DIR}/Aura.icns)
add_custom_command(OUTPUT ${AURA_ICNS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_ICONSET}
    COMMAND sips -z 16 16 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_16x16.png >/dev/null
    COMMAND sips -z 32 32 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_16x16@2x.png >/dev/null
    COMMAND sips -z 32 32 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_32x32.png >/dev/null
    COMMAND sips -z 64 64 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_32x32@2x.png >/dev/null
    COMMAND sips -z 128 128 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_128x128.png >/dev/null
    COMMAND sips -z 256 256 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_128x128@2x.png >/dev/null
    COMMAND sips -z 256 256 ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png --out ${AURA_ICONSET}/icon_256x256.png >/dev/null
    COMMAND iconutil -c icns ${AURA_ICONSET} -o ${AURA_ICNS}
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/aura.png
    VERBATIM)
set_source_files_properties(${AURA_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

add_executable(aura_cocoa MACOSX_BUNDLE
    main.m
    window.m
    input.m
    menus.m
    host.m
    net_cocoa.m
    files_cocoa.m
    fonts_cocoa.m
    downloads_cocoa.m
    sysinfo_cocoa.m
    appearance_cocoa.m
    control_cocoa.m
    ${AURA_COCOA_WINDOW_SOURCES}
    ${AURA_ICNS})
set_target_properties(aura_cocoa PROPERTIES
    OUTPUT_NAME Aura
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
target_compile_options(aura_cocoa PRIVATE -fobjc-arc)
target_link_libraries(aura_cocoa aura_posix aura_engine
    "-framework AppKit" "-framework CoreText" "-framework QuartzCore" "-framework UniformTypeIdentifiers")
target_include_directories(aura_cocoa PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aura_cocoa PRIVATE
    # The assets folder tried after the bundle's own and $AURA_ASSETS
    # (files_cocoa.m, docs/cocoa.md "Profile and assets").
    AURA_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets"
    # The shell's blocks live as long as the program (src/core/core.h, "Zones").
    AU_HEAP_ZONE=1)

# The bundle carries its assets: the fonts and the root store in
# Contents/Resources/assets, where files_cocoa.m looks first.
add_custom_command(TARGET aura_cocoa POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/fonts
            $<TARGET_BUNDLE_CONTENT_DIR:aura_cocoa>/Resources/assets/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/net
            $<TARGET_BUNDLE_CONTENT_DIR:aura_cocoa>/Resources/assets/net
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE
            $<TARGET_BUNDLE_CONTENT_DIR:aura_cocoa>/Resources/LICENSE
    COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:aura_cocoa>
    VERBATIM)
