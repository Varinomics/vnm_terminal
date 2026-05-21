set(VNM_TERMINAL_SURFACE_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_terminal_surface.")

if(NOT VNM_TERMINAL_SURFACE_SOURCE_DIR)
    get_filename_component(vnm_terminal_default_surface_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_terminal_surface"
        ABSOLUTE)
    if(EXISTS "${vnm_terminal_default_surface_dir}/CMakeLists.txt")
        set(VNM_TERMINAL_SURFACE_SOURCE_DIR
            "${vnm_terminal_default_surface_dir}"
            CACHE PATH
            "Path to a source checkout of vnm_terminal_surface."
            FORCE)
    endif()
endif()

if(VNM_TERMINAL_SURFACE_SOURCE_DIR)
    set(VNM_TERMINAL_SURFACE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(VNM_TERMINAL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(VNM_TERMINAL_ENABLE_PROFILING
        "${VNM_TERMINAL_ENABLE_PROFILING}"
        CACHE BOOL
        ""
        FORCE)
    add_subdirectory(
        "${VNM_TERMINAL_SURFACE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface")
else()
    find_package(vnm_terminal_surface CONFIG REQUIRED)
endif()
