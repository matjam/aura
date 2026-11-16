cmake_minimum_required(VERSION 3.16)
project(Aura C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# The allocation counter (src/core/core.h): bytes live per source file.
option(AURA_MEM_TRACK "Count allocations per source file" OFF)
if(AURA_MEM_TRACK)
    add_compile_definitions(AU_MEM_TRACK)
    add_compile_options(-Wno-unsequenced)
endif()

# Retro68's toolchain file sets CMAKE_SYSTEM_NAME to RetroPPC (or Retro68,
# RetroCarbon). Anything else is the development host.
if(CMAKE_SYSTEM_NAME MATCHES "Retro")
    set(AURA_MACOS9 ON)
else()
    set(AURA_MACOS9 OFF)
endif()

# The engine: every .c file under src/ except the platforms. Each area
# (core, dom, html, css, layout, paint, ...) is a folder; a new file in one
# is picked up without editing this file.
file(GLOB_RECURSE AURA_ENGINE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(FILTER AURA_ENGINE_SOURCES EXCLUDE REGEX "/src/platform/")
add_library(aura_engine STATIC ${AURA_ENGINE_SOURCES})
# Sanitizer builds: check for null pointers given to memcpy and friends
# on every host, as glibc's headers make UBSan do on Linux (nullcheck.h).
if(CMAKE_C_FLAGS MATCHES "-fsanitize")
    target_compile_options(aura_engine PRIVATE -include ${CMAKE_SOURCE_DIR}/src/core/nullcheck.h)
endif()
target_include_directories(aura_engine PUBLIC src)
# The build's identity (the commit and its date) for bug reports:
# tools/buildinfo.cmake writes gen/au_build.h on every build, and only
# src/browser/sysinfo.c includes it.
add_custom_target(aura_buildinfo
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/gen/au_build.h
            -P ${CMAKE_SOURCE_DIR}/tools/buildinfo.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/gen/au_build.h
    VERBATIM)
add_dependencies(aura_engine aura_buildinfo)
target_include_directories(aura_engine PRIVATE ${CMAKE_BINARY_DIR}/gen)
# The start page's kookaburra (about:home), embedded with #embed.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/browser/page_doc.c PROPERTIES
    OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/assets/icon/kookaburra/home.png
    COMPILE_OPTIONS "--embed-dir=${CMAKE_SOURCE_DIR}/assets/icon/kookaburra")
include(third_party/quickjs.cmake)
include(third_party/mbedtls.cmake)

if(AURA_MACOS9)
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/platform/macos9/CMakeLists.txt)
        add_subdirectory(src/platform/macos9)
    endif()
else()
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/platform/host/CMakeLists.txt)
        add_subdirectory(src/platform/host)
    endif()
    enable_testing()
    # Each folder in tests/ with a CMakeLists.txt is one test area.
    file(GLOB AURA_TEST_DIRS CONFIGURE_DEPENDS tests/*/CMakeLists.txt)
    foreach(t ${AURA_TEST_DIRS})
        get_filename_component(dir ${t} DIRECTORY)
        add_subdirectory(${dir})
    endforeach()
endif()
