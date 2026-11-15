# GIF tests, included by tests/image/CMakeLists.txt. The vectors and
# their references are made by gen-gif-vectors.py and committed.
add_executable(test_gif ${CMAKE_CURRENT_SOURCE_DIR}/test_gif.c)
target_link_libraries(test_gif aura_engine)
target_include_directories(test_gif PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME image_gif COMMAND test_gif ${CMAKE_CURRENT_SOURCE_DIR}/vectors/gif)
