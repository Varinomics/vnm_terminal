set(VNM_QML_CHROME_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_qml_chrome.")

if(NOT VNM_QML_CHROME_SOURCE_DIR)
    get_filename_component(vnm_terminal_default_qml_chrome_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/../../bsd_licensed/vnm_qml_chrome"
        ABSOLUTE)
    if(EXISTS "${vnm_terminal_default_qml_chrome_dir}/CMakeLists.txt")
        set(VNM_QML_CHROME_SOURCE_DIR
            "${vnm_terminal_default_qml_chrome_dir}"
            CACHE PATH
            "Path to a source checkout of vnm_qml_chrome."
            FORCE)
    endif()
endif()

if(VNM_QML_CHROME_SOURCE_DIR)
    set(VNM_QML_CHROME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(VNM_QML_CHROME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${VNM_QML_CHROME_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/vnm_qml_chrome")
else()
    find_package(vnm_qml_chrome CONFIG REQUIRED)
endif()
