# NetTest: the network stack on Mac OS 9 in a console window (see
# nettest.c). RetroConsole is C++, so this target links with the C++
# driver.
enable_language(CXX)
add_application(NetTest nettest.c ../net_ot.c ../entropy.c ../clock_mac.c CONSOLE CREATOR AUNT)
target_include_directories(NetTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(NetTest PRIVATE -O2)
target_link_libraries(NetTest aura_engine OpenTransportLib OpenTptInternetLib m)
