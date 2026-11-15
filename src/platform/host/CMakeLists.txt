# aura-host: the headless development tool (scene to PNG, font listing,
# benchmarks). png.c is also used by tests/paint for golden images, and the
# host platform (files, fonts, the bundled Fonts folder) by the font tests.
add_library(aura_host_png STATIC png.c)
target_link_libraries(aura_host_png PUBLIC aura_engine)
target_include_directories(aura_host_png PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

add_library(aura_host_platform STATIC platform_host.c fontdir.c)
target_link_libraries(aura_host_platform PUBLIC aura_engine)
target_include_directories(aura_host_platform PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aura_host_platform PRIVATE AURA_FONTS_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")

add_executable(aura-host main.c render.c render_loader.c net_host.c net_fetch.c net_render.c image_tool.c)
target_link_libraries(aura-host aura_host_platform aura_host_png aura_engine m pthread)
target_compile_definitions(aura-host PRIVATE AURA_ROOTS_FILE="${CMAKE_SOURCE_DIR}/assets/net/aura-roots.bin")
