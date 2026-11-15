# Nested documents (iframe, frame, object, embed): layout and paint,
# input routing, script access between documents, sandbox and framing
# rules, limits, lifetime. Uses the Ahem-like test font.
add_executable(test_frames test_frames.c)
target_link_libraries(test_frames aura_engine m)
target_include_directories(test_frames PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME frames COMMAND test_frames)
target_compile_options(test_frames PRIVATE -Wno-missing-field-initializers)

# In a browser window over the fake transport of tests/net (no network).
add_executable(test_frames_net test_frames_net.c)
target_link_libraries(test_frames_net aura_fakenet m)
target_include_directories(test_frames_net PRIVATE ${CMAKE_SOURCE_DIR}/tests/net ${CMAKE_SOURCE_DIR}/src)
add_test(NAME frames_net COMMAND test_frames_net)
set_tests_properties(frames_net PROPERTIES TIMEOUT 120)
