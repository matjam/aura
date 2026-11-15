# The Aura installer: the extractor that tools/make-sea.py joins with an
# archive of build/ppc/dist/Aura to make the self-extracting application
# (see sea.c). Builds AuraSEA.bin; ./build.sh ppc runs the packer.
#
# The 'vers' resource takes its version from AU_VERSION; the parent
# folder's CMakeLists.txt works out the variables.
configure_file(sea_vers.r.in ${CMAKE_CURRENT_BINARY_DIR}/sea_vers.r @ONLY)

add_application(AuraSEA sea.c sea_archive.c sea.r ../aura_icon.r ${CMAKE_CURRENT_BINARY_DIR}/sea_vers.r
                CREATOR AuSX)
target_include_directories(AuraSEA PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(AuraSEA PRIVATE -O2)
target_link_libraries(AuraSEA aura_engine)
