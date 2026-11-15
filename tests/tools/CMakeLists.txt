# The self-tests of the Python tools that build the release: BinHex and
# MacBinary coding (tools/binhex.py: a hand-made vector and round trips),
# the self-extracting archive's packer (tools/make-sea.py: pack a folder,
# read it back as the installer does) and the Finder flag setter.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    foreach(tool binhex make-sea finder-flags)
        add_test(NAME tools_${tool}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/${tool}.py --self-test)
    endforeach()
endif()

# The installer's archive reader (src/platform/macos9/sea/sea_archive.c,
# the code the Mac OS 9 installer runs) against the packer: sea_extract
# extracts on the host what make-sea.py packed, and refuses damaged copies.
add_executable(sea_extract sea_extract.c ${CMAKE_SOURCE_DIR}/src/platform/macos9/sea/sea_archive.c)
target_link_libraries(sea_extract aura_engine)
if(Python3_FOUND)
    add_test(NAME tools_sea_extract
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_sea.py $<TARGET_FILE:sea_extract>)
endif()
