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
# -fno-strict-aliasing: QuickJS passes uint32_t * where int * is expected
# (string_getc(p, (int *)&idx), for one). Where uint32_t is unsigned long,
# as in Retro68's newlib, GCC may then assume the callee's store does not
# change the variable: on Mac OS 9 string iterators never advanced.
target_compile_options(aura_quickjs PRIVATE
    -funsigned-char -fno-strict-aliasing
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

# The prelude (src/js/prelude*.js) is compiled to bytecode at build time by
# tools/jsprelude and embedded by src/js/prelude.c. QuickJS bytecode is in
# the byte order of the machine that reads it, so the Mac OS 9 build runs
# the tool on the host with --swap (big-endian output). The cross build
# compiles the tool with the host's own C compiler; it runs no PowerPC
# code. To embed the source instead and compile it at every runtime start
# (errors in the prelude then carry line numbers), configure with
# -DAURA_JS_PRELUDE_SOURCE=ON.
option(AURA_JS_PRELUDE_SOURCE "Embed the JavaScript prelude as source, not bytecode" OFF)
if(AURA_JS_PRELUDE_SOURCE)
    target_compile_definitions(aura_engine PRIVATE AU_JS_PRELUDE_SOURCE)
else()
    set(AURA_PRELUDE_DIR ${CMAKE_BINARY_DIR}/gen)
    set(AURA_PRELUDE_BC ${AURA_PRELUDE_DIR}/prelude.bc)
    # The order prelude.c joins the files in.
    set(AURA_PRELUDE_JS)
    foreach(part prelude prelude_dom prelude_html prelude_range prelude_cssom prelude_window prelude_web prelude_net prelude_canvas prelude_workers prelude_obs)
        list(APPEND AURA_PRELUDE_JS ${CMAKE_SOURCE_DIR}/src/js/${part}.js)
    endforeach()
    set(AURA_JSPRELUDE_SRC ${CMAKE_SOURCE_DIR}/tools/jsprelude/jsprelude.c)
    set(AURA_QJS_SOURCES ${AURA_QJS_DIR}/dtoa.c ${AURA_QJS_DIR}/libregexp.c ${AURA_QJS_DIR}/libunicode.c
        ${AURA_QJS_DIR}/quickjs.c)
    file(GLOB AURA_QJS_HEADERS ${AURA_QJS_DIR}/*.h)
    if(AURA_MACOS9)
        find_program(AURA_HOST_CC NAMES cc gcc clang NO_CMAKE_FIND_ROOT_PATH)
        if(NOT AURA_HOST_CC)
            message(FATAL_ERROR "no host C compiler (cc) to build tools/jsprelude; set AURA_HOST_CC")
        endif()
        set(AURA_JSPRELUDE ${AURA_PRELUDE_DIR}/jsprelude-host)
        add_custom_command(OUTPUT ${AURA_JSPRELUDE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_PRELUDE_DIR}
            COMMAND ${AURA_HOST_CC} -O1 -w -funsigned-char -fno-strict-aliasing -D_GNU_SOURCE -I${AURA_QJS_DIR} -o ${AURA_JSPRELUDE} ${AURA_JSPRELUDE_SRC} ${AURA_QJS_SOURCES} -lm
            DEPENDS ${AURA_JSPRELUDE_SRC} ${AURA_QJS_SOURCES} ${AURA_QJS_HEADERS}
            COMMENT "Building jsprelude with the host compiler"
            VERBATIM)
        set(AURA_JSPRELUDE_FLAGS --swap)
        set(AURA_JSPRELUDE_DEP ${AURA_JSPRELUDE})
    else()
        add_executable(jsprelude ${AURA_JSPRELUDE_SRC})
        target_link_libraries(jsprelude PRIVATE aura_quickjs)
        set(AURA_JSPRELUDE $<TARGET_FILE:jsprelude>)
        set(AURA_JSPRELUDE_FLAGS)
        set(AURA_JSPRELUDE_DEP jsprelude)
    endif()
    add_custom_command(OUTPUT ${AURA_PRELUDE_BC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_PRELUDE_DIR}
        COMMAND ${AURA_JSPRELUDE} ${AURA_JSPRELUDE_FLAGS} -o ${AURA_PRELUDE_BC} ${AURA_PRELUDE_JS}
        DEPENDS ${AURA_JSPRELUDE_DEP} ${AURA_PRELUDE_JS}
        COMMENT "Compiling the JavaScript prelude to bytecode"
        VERBATIM)
    # A worker's prelude: the parts that need no document, then the worker
    # global (prelude.c joins the same files for the source build).
    set(AURA_WORKER_BC ${AURA_PRELUDE_DIR}/prelude_worker.bc)
    set(AURA_WORKER_JS)
    foreach(part prelude prelude_web prelude_net prelude_worker)
        list(APPEND AURA_WORKER_JS ${CMAKE_SOURCE_DIR}/src/js/${part}.js)
    endforeach()
    add_custom_command(OUTPUT ${AURA_WORKER_BC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_PRELUDE_DIR}
        COMMAND ${AURA_JSPRELUDE} ${AURA_JSPRELUDE_FLAGS} -o ${AURA_WORKER_BC} ${AURA_WORKER_JS}
        DEPENDS ${AURA_JSPRELUDE_DEP} ${AURA_WORKER_JS}
        COMMENT "Compiling the worker prelude to bytecode"
        VERBATIM)
    # Intl (src/js/intl.js): its own bytecode, read the first time a page
    # touches Intl (N.intl in runtime.c), by pages and workers alike.
    set(AURA_INTL_BC ${AURA_PRELUDE_DIR}/intl.bc)
    set(AURA_INTL_JS ${CMAKE_SOURCE_DIR}/src/js/intl.js)
    add_custom_command(OUTPUT ${AURA_INTL_BC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_PRELUDE_DIR}
        COMMAND ${AURA_JSPRELUDE} ${AURA_JSPRELUDE_FLAGS} -o ${AURA_INTL_BC} ${AURA_INTL_JS}
        DEPENDS ${AURA_JSPRELUDE_DEP} ${AURA_INTL_JS}
        COMMENT "Compiling Intl to bytecode"
        VERBATIM)
    # crypto.subtle (src/js/subtle.js): the same, read the first time script
    # touches crypto.subtle (N.subtle in natives_crypto.c).
    set(AURA_SUBTLE_BC ${AURA_PRELUDE_DIR}/subtle.bc)
    set(AURA_SUBTLE_JS ${CMAKE_SOURCE_DIR}/src/js/subtle.js)
    add_custom_command(OUTPUT ${AURA_SUBTLE_BC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AURA_PRELUDE_DIR}
        COMMAND ${AURA_JSPRELUDE} ${AURA_JSPRELUDE_FLAGS} -o ${AURA_SUBTLE_BC} ${AURA_SUBTLE_JS}
        DEPENDS ${AURA_JSPRELUDE_DEP} ${AURA_SUBTLE_JS}
        COMMENT "Compiling crypto.subtle to bytecode"
        VERBATIM)
    add_custom_target(aura_prelude_bc DEPENDS ${AURA_PRELUDE_BC} ${AURA_WORKER_BC} ${AURA_INTL_BC} ${AURA_SUBTLE_BC})
    add_dependencies(aura_engine aura_prelude_bc)
    # #embed looks beside the source file, then in --embed-dir (Clang and
    # GCC 15 or later), not in the include path.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/js/prelude.c PROPERTIES
        OBJECT_DEPENDS "${AURA_PRELUDE_BC};${AURA_WORKER_BC};${AURA_INTL_BC};${AURA_SUBTLE_BC}"
        COMPILE_OPTIONS "--embed-dir=${AURA_PRELUDE_DIR}")
endif()
