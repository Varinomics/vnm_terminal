set(VNM_TERMINAL_SURFACE_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_terminal_surface.")

set(VNM_TERMINAL_SURFACE_MIN_VERSION "${PROJECT_VERSION}")

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
    set(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY
        "${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}"
        CACHE BOOL
        ""
        FORCE)
    set(VNM_TERMINAL_DISTRIBUTION_BUILD
        "${VNM_TERMINAL_DISTRIBUTION_BUILD}"
        CACHE BOOL
        ""
        FORCE)
    add_subdirectory(
        "${VNM_TERMINAL_SURFACE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface")

    get_directory_property(vnm_terminal_surface_source_version
        DIRECTORY "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface"
        DEFINITION vnm_terminal_surface_VERSION)
    get_directory_property(vnm_terminal_surface_source_version_major
        DIRECTORY "${CMAKE_BINARY_DIR}/_deps/vnm_terminal_surface"
        DEFINITION vnm_terminal_surface_VERSION_MAJOR)

    if(NOT vnm_terminal_surface_source_version)
        message(FATAL_ERROR
            "vnm_terminal_surface source checkout did not declare a project "
            "version: ${VNM_TERMINAL_SURFACE_SOURCE_DIR}")
    endif()

    if(NOT "${vnm_terminal_surface_source_version_major}" STREQUAL
        "${PROJECT_VERSION_MAJOR}")
        message(FATAL_ERROR
            "vnm_terminal_surface source checkout version "
            "${vnm_terminal_surface_source_version} is not same-major "
            "compatible with vnm_terminal ${PROJECT_VERSION}: "
            "${VNM_TERMINAL_SURFACE_SOURCE_DIR}")
    endif()

    if("${vnm_terminal_surface_source_version}" VERSION_LESS
        "${VNM_TERMINAL_SURFACE_MIN_VERSION}")
        message(FATAL_ERROR
            "vnm_terminal_surface source checkout version "
            "${vnm_terminal_surface_source_version} is older than the "
            "required minimum ${VNM_TERMINAL_SURFACE_MIN_VERSION}: "
            "${VNM_TERMINAL_SURFACE_SOURCE_DIR}")
    endif()
else()
    find_package(vnm_terminal_surface ${VNM_TERMINAL_SURFACE_MIN_VERSION}
        CONFIG REQUIRED)
endif()
