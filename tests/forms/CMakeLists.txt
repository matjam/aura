# Form controls: the editing model, entry lists and encodings, focus
# order, radio groups, selects, submission, validation and script event
# order through the document view. Uses the Ahem-like test font.
add_executable(test_forms test_forms.c)
target_link_libraries(test_forms aura_engine m)
target_include_directories(test_forms PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME forms COMMAND test_forms)

# Input to every control while a listener for one event destroys the
# target, its form, an ancestor, the body, the document or the frame
# (test_input_lifetime.c). It finds uses of freed nodes, so it earns its
# time in the sanitizer tree.
add_executable(test_input_lifetime test_input_lifetime.c)
target_link_libraries(test_input_lifetime aura_engine m)
target_include_directories(test_input_lifetime PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME forms_input_lifetime COMMAND test_input_lifetime)
set_tests_properties(forms_input_lifetime PROPERTIES TIMEOUT 600)
