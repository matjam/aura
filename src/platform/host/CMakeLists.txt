# aura-host: the headless development tool (scene to PNG, font listing,
# benchmarks). png.c is also used by tests/paint for golden images.
add_library(aura_host_png STATIC png.c)
target_link_libraries(aura_host_png PUBLIC aura_engine)
target_include_directories(aura_host_png PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(aura-host main.c platform_host.c render.c render_loader.c)
target_link_libraries(aura-host aura_host_png aura_engine m)
