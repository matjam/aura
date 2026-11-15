# JSTest: the JavaScript engine's self-test and benchmarks on Mac OS 9
# (see jstest.c). Builds JSTest.bin for tools/guest-push.sh.
add_application(JSTest jstest.c jstest.r CREATOR AUJT)
target_include_directories(JSTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(JSTest PRIVATE -O2)
target_link_libraries(JSTest aura_engine m)
