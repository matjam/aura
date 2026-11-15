# Form controls: the editing model, entry lists and encodings, focus
# order, radio groups, selects, submission, validation and script event
# order through the document view. Uses the Ahem-like test font.
add_executable(test_forms test_forms.c)
target_link_libraries(test_forms aura_engine m)
target_include_directories(test_forms PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME forms COMMAND test_forms)
