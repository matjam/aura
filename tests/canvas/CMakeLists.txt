# The canvas element and its 2D context.
#
# test_canvas2d: the context in C (src/paint/canvas2d.h): the byte-order
# conversions, every composite operator against the specification's
# formulas, geometry, state, limits, the PNG encoder, text.
add_executable(test_canvas2d test_canvas2d.c)
target_link_libraries(test_canvas2d aura_host_platform aura_engine m)
add_test(NAME canvas_2d COMMAND test_canvas2d)

# Random call sequences with hostile numbers. A short run here; longer
# under the sanitizer build: fuzz_canvas2d 1000000 SEED 600.
add_executable(fuzz_canvas2d fuzz_canvas2d.c)
target_link_libraries(fuzz_canvas2d aura_host_platform aura_engine m)
add_test(NAME canvas_fuzz COMMAND fuzz_canvas2d 3000 7 20)
set_tests_properties(canvas_fuzz PROPERTIES TIMEOUT 120)

# Canvas elements in a document view over made-up http origins: painting,
# repaints, layout cost, memory, the origin-clean rule.
add_executable(test_canvas_page test_canvas_page.c)
target_link_libraries(test_canvas_page aura_host_platform aura_engine m)
add_test(NAME canvas_page COMMAND test_canvas_page)
target_compile_options(test_canvas_page PRIVATE -Wno-missing-field-initializers)

# Script pages run by `aura-host page` (tests/js/harness.js), each fed in
# 256-byte chunks and one byte at a time.
file(GLOB CANVAS_PAGES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pages/*.html)
foreach(page ${CANVAS_PAGES})
    get_filename_component(stem ${page} NAME_WE)
    add_test(NAME canvas_page_${stem} COMMAND $<TARGET_FILE:aura-host> page --quiet ${page})
    add_test(NAME canvas_page_${stem}_bytewise COMMAND $<TARGET_FILE:aura-host> page --quiet --chunk 1 ${page})
endforeach()

# Reference pictures: each canvas of ref/scenes.html, as the page paints
# it, against golden/ID.png (made by this tool and checked by eye against
# the description beside each scene in scenes.html). To write them again
# after an intended change:
#   AURA_UPDATE_GOLDEN=1 build/host/src/platform/host/aura-host page \
#       --ref shapes tests/canvas/golden/shapes.png ... tests/canvas/ref/scenes.html
set(CANVAS_REFS)
foreach(id shapes strokes gradients compositing text images)
    list(APPEND CANVAS_REFS --ref ${id} ${CMAKE_CURRENT_SOURCE_DIR}/golden/${id}.png)
endforeach()
add_test(NAME canvas_ref COMMAND $<TARGET_FILE:aura-host> page --quiet ${CANVAS_REFS}
         ${CMAKE_CURRENT_SOURCE_DIR}/ref/scenes.html)
