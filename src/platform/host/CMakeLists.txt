# aura-host: the headless development and test tool (scene to PNG, font
# listing, benchmarks, render, fetch, js, page, chrome, blocklist). Its OS
# services are src/platform/posix (aura_posix). png.c is also used by
# tests/paint for golden images, the host platform (fonts, the bundled
# Fonts folder, the window surface) by the font tests, and render_loader.c
# by tests/layout.
add_library(aura_host_png STATIC png.c)
target_link_libraries(aura_host_png PUBLIC aura_engine)
target_include_directories(aura_host_png PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

add_library(aura_host_platform STATIC platform_host.c)
target_link_libraries(aura_host_platform PUBLIC aura_posix aura_engine)
target_include_directories(aura_host_platform PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aura_host_platform PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")

add_executable(aura-host main.c render.c render_loader.c net_host.c net_fetch.c net_render.c image_tool.c
    jsrun.c pagerun.c devtools_host.c blocklist_tool.c)
target_link_libraries(aura-host aura_host_platform aura_host_png aura_posix aura_engine m)
target_compile_definitions(aura-host PRIVATE AURA_ROOTS_FILE="${CMAKE_SOURCE_DIR}/assets/net/aura-roots.bin")
# The host shell's blocks live as long as the tool (src/core/core.h, "Zones").
target_compile_definitions(aura_host_platform PRIVATE AU_HEAP_ZONE=1)
target_compile_definitions(aura-host PRIVATE AU_HEAP_ZONE=1)
