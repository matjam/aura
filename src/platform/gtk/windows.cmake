# The GTK4 port's secondary windows (windows_gtk.h): Preferences, the
# lists, the developer windows and the About box. src/platform/gtk/
# CMakeLists.txt includes this file and adds the sources to the
# application:
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake)
#   target_sources(aura_gtk PRIVATE ${AURA_GTK_WINDOW_SOURCES})
set(AURA_GTK_WINDOW_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/winutil_gtk.c
    ${CMAKE_CURRENT_LIST_DIR}/about_gtk.c
    ${CMAKE_CURRENT_LIST_DIR}/listwin_gtk.c
    ${CMAKE_CURRENT_LIST_DIR}/prefswin_gtk.c
    ${CMAKE_CURRENT_LIST_DIR}/devwin_gtk.c
)
