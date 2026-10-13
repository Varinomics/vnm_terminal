cmake_minimum_required(VERSION 3.21)

project(varinomics_bzip2_static C)

if(NOT DEFINED BZIP2_SOURCE_DIR OR BZIP2_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "BZIP2_SOURCE_DIR is required.")
endif()
cmake_path(CONVERT "${BZIP2_SOURCE_DIR}" TO_CMAKE_PATH_LIST BZIP2_SOURCE_DIR
    NORMALIZE)

set(_bzip2_sources
    blocksort.c
    huffman.c
    crctable.c
    randtable.c
    compress.c
    decompress.c
    bzlib.c)
list(TRANSFORM _bzip2_sources PREPEND "${BZIP2_SOURCE_DIR}/")

add_library(bz2 STATIC ${_bzip2_sources})
target_compile_definitions(bz2 PRIVATE WIN32 _FILE_OFFSET_BITS=64)
target_include_directories(bz2 PUBLIC "${BZIP2_SOURCE_DIR}")
set_target_properties(bz2 PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    OUTPUT_NAME bz2)

install(TARGETS bz2 ARCHIVE DESTINATION lib)
install(FILES "${BZIP2_SOURCE_DIR}/bzlib.h" DESTINATION include)
