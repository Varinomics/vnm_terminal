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
    "VNM_TERMINAL_FRAMEWORK_SOURCE_DIR")
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

string(CONCAT local_fixture
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(local_provider LANGUAGES NONE)\n"
    "include(\"${dependency_file}\")\n"
    "vnm_terminal_environment_policy_make_available()\n"
    "if(NOT VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER STREQUAL \"terminal-local substitute\")\n"
    "  message(FATAL_ERROR \"local provider was not selected\")\n"
    "endif()\n"
    "get_target_property(provider vnm_terminal_environment_policy VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER)\n"
    "if(NOT provider STREQUAL \"terminal-local substitute\")\n"
    "  message(FATAL_ERROR \"local target provenance is missing\")\n"
    "endif()\n"
    "get_target_property(links vnm_terminal_environment_policy INTERFACE_LINK_LIBRARIES)\n"
    "if(NOT \"vnm_terminal_local_environment_policy\" IN_LIST links)\n"
    "  message(FATAL_ERROR \"selected provider does not link the local substitute\")\n"
    "endif()\n"
    "get_target_property(definitions vnm_terminal_local_environment_policy INTERFACE_COMPILE_DEFINITIONS)\n"
    "if(NOT \"VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK=0\" IN_LIST definitions)\n"
    "  message(FATAL_ERROR \"local provider definition is missing\")\n"
    "endif()\n"
    "get_target_property(sources vnm_terminal_local_environment_policy INTERFACE_SOURCES)\n"
    "if(NOT sources MATCHES \"local_environment_policy\\\\.cpp\")\n"
    "  message(FATAL_ERROR \"local substitute source is missing\")\n"
    "endif()\n")
configure_fixture(local_provider "${local_fixture}")

string(CONCAT framework_fixture
    "cmake_minimum_required(VERSION 3.21)\n"
    "project(framework_provider LANGUAGES NONE)\n"
    "add_library(vnm_framework::vnm_environment_policy INTERFACE IMPORTED)\n"
    "include(\"${dependency_file}\")\n"
    "vnm_terminal_environment_policy_make_available()\n"
    "if(NOT VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER STREQUAL \"vnm_framework\")\n"
    "  message(FATAL_ERROR \"framework provider was not selected\")\n"
    "endif()\n"
    "get_target_property(provider vnm_terminal_environment_policy VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER)\n"
    "if(NOT provider STREQUAL \"vnm_framework\")\n"
    "  message(FATAL_ERROR \"framework target provenance is missing\")\n"
    "endif()\n"
    "get_target_property(definitions vnm_terminal_environment_policy INTERFACE_COMPILE_DEFINITIONS)\n"
    "if(NOT \"VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK=1\" IN_LIST definitions)\n"
    "  message(FATAL_ERROR \"framework provider definition is missing\")\n"
    "endif()\n"
    "get_target_property(links vnm_terminal_environment_policy INTERFACE_LINK_LIBRARIES)\n"
    "if(NOT \"vnm_framework::vnm_environment_policy\" IN_LIST links)\n"
    "  message(FATAL_ERROR \"framework target is not linked\")\n"
    "endif()\n"
    "get_target_property(sources vnm_terminal_environment_policy INTERFACE_SOURCES)\n"
    "if(sources)\n"
    "  message(FATAL_ERROR \"framework provider retained local sources\")\n"
    "endif()\n"
    "get_target_property(local_definitions vnm_terminal_local_environment_policy INTERFACE_COMPILE_DEFINITIONS)\n"
    "if(NOT \"VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK=0\" IN_LIST local_definitions)\n"
    "  message(FATAL_ERROR \"local conformance provider definition is missing\")\n"
    "endif()\n"
    "get_target_property(local_sources vnm_terminal_local_environment_policy INTERFACE_SOURCES)\n"
    "if(NOT local_sources MATCHES \"local_environment_policy\\\\.cpp\")\n"
    "  message(FATAL_ERROR \"local conformance provider source is missing\")\n"
    "endif()\n")
configure_fixture(framework_provider "${framework_fixture}")

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
message(STATUS "Environment-policy provider contract passed")
