# QuickJS-ng, the JavaScript engine (third_party/quickjs, see
# third_party/README.md). Included from the top-level CMakeLists.txt after
# the aura_engine library is defined; builds the engine files only (no
# CLI, no libc module) as a static library and links it into aura_engine.
#
# The library keeps QuickJS-ng's own warning set (it does not build clean
# under Aura's -Wall -Wextra), and -funsigned-char as upstream requires.
# AURA_QJS_DEFINES carries the size trims (see third_party/README.md).
set(AURA_QJS_DIR ${CMAKE_CURRENT_LIST_DIR}/quickjs)
add_library(aura_quickjs STATIC
    ${AURA_QJS_DIR}/dtoa.c
    ${AURA_QJS_DIR}/libregexp.c
    ${AURA_QJS_DIR}/libunicode.c
    ${AURA_QJS_DIR}/quickjs.c)
target_include_directories(aura_quickjs SYSTEM PUBLIC ${AURA_QJS_DIR})
target_compile_options(aura_quickjs PRIVATE
    -funsigned-char
    -Wno-implicit-fallthrough -Wno-sign-compare -Wno-missing-field-initializers
    -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-result
    -Wno-array-bounds -Wno-unused-variable -Wno-unused-function
    -Wno-format -Wno-cast-function-type)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(aura_quickjs PRIVATE -Wno-stringop-truncation -Wno-maybe-uninitialized)
endif()
if(AURA_MACOS9)
    target_compile_options(aura_quickjs PRIVATE -O2)
    target_compile_definitions(aura_quickjs PRIVATE NDEBUG)
endif()
target_compile_definitions(aura_quickjs PRIVATE _GNU_SOURCE ${AURA_QJS_DEFINES})
target_link_libraries(aura_engine PUBLIC aura_quickjs)
# The math library goes after QuickJS on the link line (newlib's libm on
# Mac OS 9 holds cbrt, hypot, the hyperbolic functions and so on).
if(NOT APPLE OR AURA_MACOS9)
    target_link_libraries(aura_quickjs PUBLIC m)
endif()
