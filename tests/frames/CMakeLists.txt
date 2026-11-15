# Nested documents (iframe, frame, object, embed): layout and paint,
# input routing, script access between documents, sandbox and framing
# rules, limits, lifetime. Uses the Ahem-like test font.
add_executable(test_frames test_frames.c)
target_link_libraries(test_frames aura_engine m)
target_include_directories(test_frames PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME frames COMMAND test_frames)
target_compile_options(test_frames PRIVATE -Wno-missing-field-initializers)
