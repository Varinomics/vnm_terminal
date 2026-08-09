if(NOT VNM_TERMINAL_SOURCE_ROOT OR
    NOT VNM_TERMINAL_TEST_ROOT OR
    NOT VNM_TERMINAL_VERSION)
    message(FATAL_ERROR "Source root, test root, and terminal version are required")
endif()

file(TO_CMAKE_PATH "${VNM_TERMINAL_SOURCE_ROOT}" source_root)
file(TO_CMAKE_PATH "${VNM_TERMINAL_TEST_ROOT}" test_root)
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

function(configure_with_chrome provider_version out_result out_log)
    string(REPLACE "." "_" provider_name "${provider_version}")
    set(provider_dir "${test_root}/provider_${provider_name}")
    set(probe_dir "${test_root}/probe_${provider_name}")
    set(build_dir "${test_root}/build_${provider_name}")
    file(MAKE_DIRECTORY "${provider_dir}" "${probe_dir}")

    file(WRITE "${provider_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(vnm_qml_chrome LANGUAGES NONE VERSION ${provider_version})\n"
        "add_library(vnm_qml_chrome INTERFACE)\n"
        "add_library(vnm_qml_chrome::vnm_qml_chrome ALIAS vnm_qml_chrome)\n")

    file(WRITE "${probe_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(vnm_terminal LANGUAGES NONE VERSION ${VNM_TERMINAL_VERSION})\n"
        "set(VNM_QML_CHROME_SOURCE_DIR \"${provider_dir}\")\n"
        "include(\"${source_root}/cmake/vnm_qml_chrome_dependency.cmake\")\n")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${probe_dir}" -B "${build_dir}"
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error)

    set(${out_result} "${configure_result}" PARENT_SCOPE)
    set(${out_log} "${configure_output}\n${configure_error}" PARENT_SCOPE)
endfunction()

configure_with_chrome("1.4.0" older_provider_result older_provider_log)
if(older_provider_result EQUAL 0)
    message(FATAL_ERROR "vnm_terminal accepted vnm_qml_chrome 1.4.0")
endif()
string(REGEX REPLACE "[\r\n\t ]+" " " older_provider_log "${older_provider_log}")
if(NOT older_provider_log MATCHES "older than the required minimum 1\\.5")
    message(FATAL_ERROR
        "Older-provider rejection did not name the required 1.5 floor:\n"
        "${older_provider_log}")
endif()

configure_with_chrome("1.5.0" minimum_provider_result minimum_provider_log)
if(NOT minimum_provider_result EQUAL 0)
    message(FATAL_ERROR
        "vnm_terminal rejected vnm_qml_chrome 1.5.0:\n"
        "${minimum_provider_log}")
endif()
