# Image cache tests, included by tests/image/CMakeLists.txt.
add_executable(test_image_cache ${CMAKE_CURRENT_SOURCE_DIR}/test_cache.c)
target_link_libraries(test_image_cache aura_engine)
target_include_directories(test_image_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_cache COMMAND test_image_cache ${CMAKE_CURRENT_SOURCE_DIR}/vectors)
