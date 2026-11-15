# Image drawing tests (au_draw_image), included by tests/image/CMakeLists.txt.
add_executable(test_image_draw ${CMAKE_CURRENT_SOURCE_DIR}/test_draw.c)
target_link_libraries(test_image_draw aura_engine)
target_include_directories(test_image_draw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_draw COMMAND test_image_draw)
