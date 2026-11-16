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

# The platform to build (docs/porting.md):
#   macos9  the Mac OS 9 application (src/platform/macos9), with Retro68
#   host    the aura-host tool and the tests (src/platform/host), on macOS
#           or Linux
#   gtk     the Linux GTK4 port (src/platform/gtk); reserved, not written
# Retro68's toolchain file sets CMAKE_SYSTEM_NAME to RetroPPC (or Retro68,
# RetroCarbon), and the default follows it, so ./build.sh needs no option.
if(CMAKE_SYSTEM_NAME MATCHES "Retro")
    set(AURA_PLATFORM_DEFAULT macos9)
else()
    set(AURA_PLATFORM_DEFAULT host)
endif()
set(AURA_PLATFORM ${AURA_PLATFORM_DEFAULT} CACHE STRING "Platform to build: macos9, host or gtk")
set_property(CACHE AURA_PLATFORM PROPERTY STRINGS macos9 host gtk)
if(AURA_PLATFORM STREQUAL "gtk")
    message(FATAL_ERROR "AURA_PLATFORM=gtk: the GTK4 port is not written yet. "
        "docs/porting.md describes it and the order of the work. Configure with "
        "AURA_PLATFORM=host for the host tool and the tests.")
elseif(AURA_PLATFORM STREQUAL "macos9")
    if(NOT CMAKE_SYSTEM_NAME MATCHES "Retro")
        message(FATAL_ERROR "AURA_PLATFORM=macos9 needs the Retro68 toolchain file: "
            "run ./build.sh ppc, or configure an empty folder with "
            "-DCMAKE_TOOLCHAIN_FILE=.../retroppc.toolchain.cmake (docs/building.md).")
    endif()
elseif(AURA_PLATFORM STREQUAL "host")
    if(CMAKE_SYSTEM_NAME MATCHES "Retro")
        message(FATAL_ERROR "AURA_PLATFORM=host cannot be built with the Retro68 toolchain; "
            "use AURA_PLATFORM=macos9 or leave it unset.")
    endif()
else()
    message(FATAL_ERROR "AURA_PLATFORM=${AURA_PLATFORM}: use macos9, host or gtk (docs/building.md).")
endif()
# Derived from AURA_PLATFORM for the files that test it
# (third_party/quickjs.cmake, third_party/mbedtls.cmake).
if(AURA_PLATFORM STREQUAL "macos9")
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
# Zones (src/core/core.h, docs/memory.md "Zones"): blocks allocated in
# these files outlive pages and go to the long-lived zone; the others go
# to the page zone. src/core, src/codec and src/url allocate for their
# callers and take the caller's zone. The platform shells set
# AU_HEAP_ZONE=1 for their own targets.
file(GLOB AURA_LONG_LIVED_SOURCES CONFIGURE_DEPENDS src/net/*.c)
foreach(f paint/fontset.c paint/sfnt.c paint/fontpage.c paint/fontindex.c paint/fontalias.c paint/fallback.c
          paint/fontvar.c paint/colr.c paint/gsub.c paint/text.c
          browser/browser.c browser/history.c browser/bookmarks.c browser/prefs.c browser/downloads.c browser/favicon.c)
    list(APPEND AURA_LONG_LIVED_SOURCES ${CMAKE_SOURCE_DIR}/src/${f})
endforeach()
set_property(SOURCE ${AURA_LONG_LIVED_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS AU_HEAP_ZONE=1)
file(GLOB AURA_ZONE_INHERIT_SOURCES CONFIGURE_DEPENDS src/core/*.c src/codec/*.c src/url/*.c)
set_property(SOURCE ${AURA_ZONE_INHERIT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS AU_HEAP_ZONE_INHERIT)
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

if(AURA_PLATFORM STREQUAL "macos9")
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/platform/macos9/CMakeLists.txt)
        add_subdirectory(src/platform/macos9)
    endif()
elseif(AURA_PLATFORM STREQUAL "host")
    # The POSIX services, then the tool that uses them.
    add_subdirectory(src/platform/posix)
    add_subdirectory(src/platform/host)
    enable_testing()
    # Each folder in tests/ with a CMakeLists.txt is one test area.
    file(GLOB AURA_TEST_DIRS CONFIGURE_DEPENDS tests/*/CMakeLists.txt)
    foreach(t ${AURA_TEST_DIRS})
        get_filename_component(dir ${t} DIRECTORY)
        add_subdirectory(${dir})
    endforeach()
endif()
