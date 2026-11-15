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
