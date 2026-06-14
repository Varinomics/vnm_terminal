set(VNM_QML_CHROME_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_qml_chrome.")

set(VNM_QML_CHROME_MIN_VERSION "${PROJECT_VERSION}")

if(NOT VNM_QML_CHROME_SOURCE_DIR)
    set(vnm_terminal_default_qml_chrome_dirs
        "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_qml_chrome"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../bsd_licensed/vnm_qml_chrome")
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
    get_directory_property(vnm_qml_chrome_source_version_major
        DIRECTORY "${CMAKE_BINARY_DIR}/_deps/vnm_qml_chrome"
        DEFINITION vnm_qml_chrome_VERSION_MAJOR)

    if(NOT vnm_qml_chrome_source_version)
        message(FATAL_ERROR
            "vnm_qml_chrome source checkout did not declare a project "
            "version: ${VNM_QML_CHROME_SOURCE_DIR}")
    endif()

    if(NOT "${vnm_qml_chrome_source_version_major}" STREQUAL
        "${PROJECT_VERSION_MAJOR}")
        message(FATAL_ERROR
            "vnm_qml_chrome source checkout version "
            "${vnm_qml_chrome_source_version} is not same-major "
            "compatible with vnm_terminal ${PROJECT_VERSION}: "
            "${VNM_QML_CHROME_SOURCE_DIR}")
    endif()

    if("${vnm_qml_chrome_source_version}" VERSION_LESS
        "${VNM_QML_CHROME_MIN_VERSION}")
        message(FATAL_ERROR
            "vnm_qml_chrome source checkout version "
            "${vnm_qml_chrome_source_version} is older than the "
            "required minimum ${VNM_QML_CHROME_MIN_VERSION}: "
            "${VNM_QML_CHROME_SOURCE_DIR}")
    endif()
else()
    find_package(vnm_qml_chrome ${VNM_QML_CHROME_MIN_VERSION} CONFIG REQUIRED)
endif()
