if(NOT VNM_TERMINAL_SOURCE_ROOT OR NOT VNM_TERMINAL_TEST_ROOT OR
   NOT VNM_TERMINAL_TEST_GENERATOR OR NOT VNM_TERMINAL_TEST_MAKE_PROGRAM)
    message(FATAL_ERROR
        "Environment-policy provider test roots and generator are required")
endif()

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
file(MAKE_DIRECTORY "${VNM_TERMINAL_TEST_ROOT}")

set(dependency_file
    "${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_environment_policy_dependency.cmake")
file(READ "${dependency_file}" dependency_contents)
foreach(forbidden_term IN ITEMS
    "FetchContent"
    "GIT_REPOSITORY"
    "find_package"
    "vnm_framework"
    "VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK")
    if(dependency_contents MATCHES "${forbidden_term}")
        message(FATAL_ERROR
            "Environment-policy selection must not use ${forbidden_term}")
    endif()
endforeach()

function(configure_fixture fixture_name fixture_contents)
    set(fixture_root "${VNM_TERMINAL_TEST_ROOT}/${fixture_name}")
    file(MAKE_DIRECTORY "${fixture_root}")
    file(WRITE "${fixture_root}/CMakeLists.txt" "${fixture_contents}")

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            -S "${fixture_root}"
            -B "${fixture_root}/build"
            -G "${VNM_TERMINAL_TEST_GENERATOR}"
            "-DCMAKE_MAKE_PROGRAM=${VNM_TERMINAL_TEST_MAKE_PROGRAM}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${fixture_name} provider selection failed:\n${output}\n${error}")
    endif()
endfunction()

set(fixture_template [=[
cmake_minimum_required(VERSION 3.21)
project(local_provider LANGUAGES NONE)
@parent_targets@
include("@dependency_file@")
vnm_terminal_environment_policy_make_available()
if(NOT VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER STREQUAL "terminal-local")
    message(FATAL_ERROR "Local policy was not selected")
endif()
get_target_property(provider vnm_terminal_environment_policy VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER)
if(NOT provider STREQUAL "terminal-local")
    message(FATAL_ERROR "Local target provenance is missing")
endif()
get_target_property(links vnm_terminal_environment_policy INTERFACE_LINK_LIBRARIES)
if(links)
    message(FATAL_ERROR "Terminal environment policy must not link another provider")
endif()
get_target_property(sources vnm_terminal_environment_policy INTERFACE_SOURCES)
if(NOT sources STREQUAL "@VNM_TERMINAL_SOURCE_ROOT@/cmake/../src/local_environment_policy.cpp")
    message(FATAL_ERROR "Local policy source is missing")
endif()
]=])
set(parent_targets "")
string(CONFIGURE "${fixture_template}" fixture @ONLY)
configure_fixture(standalone "${fixture}")

# An embedding parent cannot silently substitute the terminal's policy.
set(parent_targets "add_library(vnm_framework::vnm_environment_policy INTERFACE IMPORTED)")
string(CONFIGURE "${fixture_template}" fixture @ONLY)
configure_fixture(embedded "${fixture}")

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
message(STATUS "Environment-policy independence contract passed")
