if(NOT DEFINED VNM_TERMINAL_SOURCE_ROOT OR NOT DEFINED VNM_TERMINAL_TEST_ROOT)
    message(FATAL_ERROR "surface semantic dependency test roots are required")
endif()

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
file(MAKE_DIRECTORY "${VNM_TERMINAL_TEST_ROOT}")

function(configure_fixture fixture_name surface_contents expect_success)
    set(fixture_root "${VNM_TERMINAL_TEST_ROOT}/${fixture_name}")
    set(surface_root "${fixture_root}/surface")
    set(consumer_root "${fixture_root}/consumer")
    file(MAKE_DIRECTORY "${surface_root}" "${consumer_root}")
    file(WRITE "${surface_root}/CMakeLists.txt" "${surface_contents}")
    string(CONCAT consumer_contents
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(surface_consumer VERSION 1.4.6 LANGUAGES NONE)\n"
        "set(VNM_TERMINAL_SURFACE_SOURCE_DIR \"${surface_root}\" CACHE PATH \"\" FORCE)\n"
        "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_surface_dependency.cmake\")\n")
    file(WRITE "${consumer_root}/CMakeLists.txt" "${consumer_contents}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${consumer_root}" -B "${fixture_root}/build"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR
            "compatible differently-versioned surface was rejected:\n${output}\n${error}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "surface without the semantic target was accepted")
    endif()
    if(NOT expect_success AND
       NOT "${output}\n${error}" MATCHES "semantic surface capability target")
        message(FATAL_ERROR
            "missing capability failure was not clear:\n${output}\n${error}")
    endif()
endfunction()

configure_fixture(
    compatible_version_skew
    "cmake_minimum_required(VERSION 3.21)\nproject(vnm_terminal_surface VERSION 9.8.7 LANGUAGES NONE)\nadd_library(vnm_terminal_surface INTERFACE)\nadd_library(vnm_terminal_surface::vnm_terminal_surface ALIAS vnm_terminal_surface)\n"
    TRUE)
configure_fixture(
    missing_semantic_capability
    "cmake_minimum_required(VERSION 3.21)\nproject(vnm_terminal_surface VERSION 1.4.6 LANGUAGES NONE)\n"
    FALSE)

set(package_fixture_root "${VNM_TERMINAL_TEST_ROOT}/compatible_package_version_skew")
set(package_config_dir
    "${package_fixture_root}/prefix/lib/cmake/vnm_terminal_surface")
set(package_consumer_dir "${package_fixture_root}/consumer")
file(MAKE_DIRECTORY "${package_config_dir}" "${package_consumer_dir}")
file(WRITE "${package_config_dir}/vnm_terminal_surfaceConfig.cmake" [=[
add_library(vnm_terminal_surface::vnm_terminal_surface INTERFACE IMPORTED)
]=])
file(WRITE "${package_config_dir}/vnm_terminal_surfaceConfigVersion.cmake" [=[
set(PACKAGE_VERSION "42.7.3")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT FALSE)
]=])
string(CONCAT package_consumer_contents
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(surface_package_consumer VERSION 1.4.6 LANGUAGES NONE)\n"
    "list(PREPEND CMAKE_PREFIX_PATH \"${package_fixture_root}/prefix\")\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_surface_dependency.cmake\")\n")
file(WRITE
    "${package_consumer_dir}/CMakeLists.txt"
    "${package_consumer_contents}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${package_consumer_dir}"
        -B "${package_fixture_root}/build"
    RESULT_VARIABLE package_result
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR
        "compatible differently-versioned surface package was rejected:\n"
        "${package_output}\n${package_error}")
endif()

set(preexisting_fixture_root
    "${VNM_TERMINAL_TEST_ROOT}/preexisting_source_target_provenance")
set(preexisting_surface_dir "${preexisting_fixture_root}/surface")
set(preexisting_chrome_dir "${preexisting_fixture_root}/chrome")
set(preexisting_consumer_dir "${preexisting_fixture_root}/consumer")
file(MAKE_DIRECTORY
    "${preexisting_surface_dir}"
    "${preexisting_chrome_dir}"
    "${preexisting_consumer_dir}")
file(WRITE "${preexisting_surface_dir}/CMakeLists.txt" [=[
add_library(vnm_terminal_surface INTERFACE)
add_library(
    vnm_terminal_surface::vnm_terminal_surface
    ALIAS vnm_terminal_surface)
]=])
file(WRITE "${preexisting_chrome_dir}/CMakeLists.txt" [=[
add_library(vnm_qml_chrome INTERFACE)
add_library(vnm_qml_chrome::vnm_qml_chrome ALIAS vnm_qml_chrome)
]=])
string(CONCAT preexisting_consumer_contents
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(preexisting_source_target_consumer LANGUAGES NONE)\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_dependency_origin.cmake\")\n"
    "add_subdirectory(\"${preexisting_surface_dir}\" surface-build)\n"
    "add_subdirectory(\"${preexisting_chrome_dir}\" chrome-build)\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_surface_dependency.cmake\")\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_qml_chrome_dependency.cmake\")\n"
    "file(REAL_PATH \"${preexisting_surface_dir}\" expected_surface)\n"
    "file(REAL_PATH \"${preexisting_chrome_dir}\" expected_chrome)\n"
    "file(REAL_PATH \"\${VNM_TERMINAL_SURFACE_SOURCE_DIR}\" actual_surface)\n"
    "file(REAL_PATH \"\${VNM_QML_CHROME_SOURCE_DIR}\" actual_chrome)\n"
    "if(NOT actual_surface STREQUAL expected_surface)\n"
    "  message(FATAL_ERROR \"surface provenance was not adopted: "
    "\${VNM_TERMINAL_SURFACE_SOURCE_DIR}\")\n"
    "endif()\n"
    "if(NOT actual_chrome STREQUAL expected_chrome)\n"
    "  message(FATAL_ERROR \"chrome provenance was not adopted: \${VNM_QML_CHROME_SOURCE_DIR}\")\n"
    "endif()\n")
file(WRITE
    "${preexisting_consumer_dir}/CMakeLists.txt"
    "${preexisting_consumer_contents}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${preexisting_consumer_dir}"
        -B "${preexisting_fixture_root}/build"
    RESULT_VARIABLE preexisting_result
    OUTPUT_VARIABLE preexisting_output
    ERROR_VARIABLE preexisting_error)
if(NOT preexisting_result EQUAL 0)
    message(FATAL_ERROR
        "pre-existing source target provenance was not retained:\n"
        "${preexisting_output}\n${preexisting_error}")
endif()

set(imported_fixture_root
    "${VNM_TERMINAL_TEST_ROOT}/preexisting_imported_target_provenance")
set(imported_consumer_dir "${imported_fixture_root}/consumer")
file(MAKE_DIRECTORY "${imported_consumer_dir}")
string(CONCAT imported_consumer_contents
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(preexisting_imported_target_consumer LANGUAGES NONE)\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_dependency_origin.cmake\")\n"
    "set(VNM_TERMINAL_SURFACE_SOURCE_DIR \"stale-surface-cache\" CACHE PATH \"stale surface\" FORCE)\n"
    "set(VNM_QML_CHROME_SOURCE_DIR \"stale-chrome-cache\" CACHE PATH \"stale chrome\" FORCE)\n"
    "set(VNM_TERMINAL_SURFACE_SOURCE_DIR \"stale-surface-normal\")\n"
    "set(VNM_QML_CHROME_SOURCE_DIR \"stale-chrome-normal\")\n"
    "add_library(vnm_terminal_surface::vnm_terminal_surface INTERFACE IMPORTED)\n"
    "add_library(vnm_qml_chrome::vnm_qml_chrome INTERFACE IMPORTED)\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_surface_dependency.cmake\")\n"
    "include(\"${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_qml_chrome_dependency.cmake\")\n"
    "if(NOT \"\${VNM_TERMINAL_SURFACE_SOURCE_DIR}\" STREQUAL \"\")\n"
    "  message(FATAL_ERROR \"imported surface retained stale source provenance: \${VNM_TERMINAL_SURFACE_SOURCE_DIR}\")\n"
    "endif()\n"
    "if(NOT \"\${VNM_QML_CHROME_SOURCE_DIR}\" STREQUAL \"\")\n"
    "  message(FATAL_ERROR \"imported chrome retained stale source provenance: \${VNM_QML_CHROME_SOURCE_DIR}\")\n"
    "endif()\n"
    "get_property(surface_cache CACHE VNM_TERMINAL_SURFACE_SOURCE_DIR PROPERTY VALUE)\n"
    "get_property(chrome_cache CACHE VNM_QML_CHROME_SOURCE_DIR PROPERTY VALUE)\n"
    "if(NOT \"\${surface_cache}\" STREQUAL \"\")\n"
    "  message(FATAL_ERROR \"imported surface retained stale cached provenance: \${surface_cache}\")\n"
    "endif()\n"
    "if(NOT \"\${chrome_cache}\" STREQUAL \"\")\n"
    "  message(FATAL_ERROR \"imported chrome retained stale cached provenance: \${chrome_cache}\")\n"
    "endif()\n")
file(WRITE
    "${imported_consumer_dir}/CMakeLists.txt"
    "${imported_consumer_contents}")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${imported_consumer_dir}"
        -B "${imported_fixture_root}/build"
    RESULT_VARIABLE imported_result
    OUTPUT_VARIABLE imported_output
    ERROR_VARIABLE imported_error)
if(NOT imported_result EQUAL 0)
    message(FATAL_ERROR
        "pre-existing imported target provenance was not cleared:\n"
        "${imported_output}\n${imported_error}")
endif()
