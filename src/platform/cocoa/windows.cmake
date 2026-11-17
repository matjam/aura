# The secondary windows of the macOS port (windows_cocoa.h): Preferences,
# the lists, the developer windows and About. CMakeLists.txt includes this
# file. winutil_cocoa.m holds the entry points every window shares and a
# weak placeholder for each window's functions, which that window's own
# file replaces. To add a source to them, add it here.
set(AURA_COCOA_WINDOW_SOURCES winutil_cocoa.m)
foreach(f prefswin_cocoa.m listwin_cocoa.m devwin_cocoa.m about_cocoa.m)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${f})
        list(APPEND AURA_COCOA_WINDOW_SOURCES ${f})
    endif()
endforeach()
