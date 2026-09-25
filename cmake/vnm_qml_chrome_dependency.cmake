set(VNM_QML_CHROME_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_qml_chrome.")

set(VNM_QML_CHROME_MIN_VERSION "1.10.1")

function(vnm_terminal_require_qml_chrome_version)
    set(vnm_terminal_qml_chrome_target "")
    foreach(vnm_terminal_qml_chrome_candidate IN ITEMS
        vnm_qml_chrome::vnm_qml_chrome
        vnm_qml_chrome)
        if(TARGET ${vnm_terminal_qml_chrome_candidate})
            set(vnm_terminal_qml_chrome_target
                "${vnm_terminal_qml_chrome_candidate}")
            break()
        endif()
    endforeach()

    if(NOT vnm_terminal_qml_chrome_target)
        message(FATAL_ERROR
            "vnm_qml_chrome did not provide a target whose version can be "
            "checked against the required minimum ${VNM_QML_CHROME_MIN_VERSION}.")
    endif()

    get_target_property(vnm_terminal_qml_chrome_version
        ${vnm_terminal_qml_chrome_target}
        VNM_QML_CHROME_VERSION)
    if(NOT vnm_terminal_qml_chrome_version)
        message(FATAL_ERROR
            "The existing ${vnm_terminal_qml_chrome_target} target does not "
            "publish VNM_QML_CHROME_VERSION; the required minimum "
            "${VNM_QML_CHROME_MIN_VERSION} cannot be verified.")
    endif()
    if(vnm_terminal_qml_chrome_version VERSION_LESS
        VNM_QML_CHROME_MIN_VERSION)
        message(FATAL_ERROR
            "vnm_qml_chrome ${vnm_terminal_qml_chrome_version} is older than "
            "the required minimum ${VNM_QML_CHROME_MIN_VERSION}.")
    endif()
endfunction()

if(TARGET vnm_qml_chrome::vnm_qml_chrome)
    vnm_terminal_adopt_existing_target_source(
        vnm_qml_chrome::vnm_qml_chrome
        VNM_QML_CHROME_SOURCE_DIR)
    vnm_terminal_require_qml_chrome_version()
    return()
elseif(TARGET vnm_qml_chrome)
    vnm_terminal_adopt_existing_target_source(
        vnm_qml_chrome
        VNM_QML_CHROME_SOURCE_DIR)
    vnm_terminal_require_qml_chrome_version()
    return()
endif()

if(NOT VNM_QML_CHROME_SOURCE_DIR)
    # Both candidates are siblings of this repository: the checkout may sit
    # beside it, or inside a sibling directory that groups the BSD-licensed
    # dependencies. `..` is therefore the last segment either path may drop.
    set(vnm_terminal_default_qml_chrome_dirs
        "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_qml_chrome"
        "${CMAKE_CURRENT_SOURCE_DIR}/../bsd_licensed/vnm_qml_chrome")
    foreach(vnm_terminal_default_qml_chrome_candidate IN LISTS
            vnm_terminal_default_qml_chrome_dirs)
        get_filename_component(vnm_terminal_default_qml_chrome_dir
            "${vnm_terminal_default_qml_chrome_candidate}"
            ABSOLUTE)
        if(EXISTS "${vnm_terminal_default_qml_chrome_dir}/CMakeLists.txt")
            set(VNM_QML_CHROME_SOURCE_DIR
                "${vnm_terminal_default_qml_chrome_dir}"
                CACHE PATH
                "Path to a source checkout of vnm_qml_chrome."
                FORCE)
            break()
        endif()
    endforeach()
endif()

if(VNM_QML_CHROME_SOURCE_DIR)
    set(VNM_QML_CHROME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(VNM_QML_CHROME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${VNM_QML_CHROME_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/vnm_qml_chrome")

    get_directory_property(vnm_qml_chrome_source_version
        DIRECTORY "${CMAKE_BINARY_DIR}/_deps/vnm_qml_chrome"
        DEFINITION vnm_qml_chrome_VERSION)
    if(NOT vnm_qml_chrome_source_version)
        message(FATAL_ERROR
            "vnm_qml_chrome source checkout did not declare a project "
            "version: ${VNM_QML_CHROME_SOURCE_DIR}")
    endif()

    if("${vnm_qml_chrome_source_version}" VERSION_LESS
        "${VNM_QML_CHROME_MIN_VERSION}")
        message(FATAL_ERROR
            "vnm_qml_chrome source checkout version "
            "${vnm_qml_chrome_source_version} is older than the "
            "required minimum ${VNM_QML_CHROME_MIN_VERSION}: "
            "${VNM_QML_CHROME_SOURCE_DIR}")
    endif()

    vnm_terminal_require_qml_chrome_version()
else()
    find_package(vnm_qml_chrome CONFIG REQUIRED)
    vnm_terminal_require_qml_chrome_version()
endif()
