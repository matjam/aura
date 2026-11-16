# The POSIX services a Unix port reuses (posix.h): sockets and DNS, files
# and the application files over a folder, clocks, entropy, coroutines and
# font files. The host tool (src/platform/host) links it, and so will the
# GTK4 port. It has no event loop and no dependency on either.
add_library(aura_posix STATIC
    clock_posix.c
    coro_posix.c
    entropy_posix.c
    files_posix.c
    fontdir_posix.c
    net_posix.c)
find_package(Threads REQUIRED)
target_link_libraries(aura_posix PUBLIC aura_engine Threads::Threads)
target_include_directories(aura_posix PUBLIC ${CMAKE_SOURCE_DIR}/src)
# Its blocks (font folders, loaded font files) live as long as the
# program (src/core/core.h, "Zones").
target_compile_definitions(aura_posix PRIVATE AU_HEAP_ZONE=1)
