if(NOT DEFINED expected_exit_code)
    message(FATAL_ERROR "expected_exit_code is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/vnm_terminal_cmake_script_helpers.cmake")

set(stderr_must_be_empty OFF)
if(DEFINED require_empty_stderr)
    set(stderr_must_be_empty "${require_empty_stderr}")
endif()

vnm_terminal_script_command_args(command_args)

execute_process(
    COMMAND ${command_args}
    RESULT_VARIABLE actual_exit_code
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
)

if(NOT actual_exit_code STREQUAL expected_exit_code)
    message(FATAL_ERROR
        "expected exit code ${expected_exit_code}, got ${actual_exit_code}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

if(stderr_must_be_empty AND NOT stderr_text STREQUAL "")
    message(FATAL_ERROR
        "expected empty stderr\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

if(DEFINED expected_stdout_regex AND NOT stdout_text MATCHES "${expected_stdout_regex}")
    message(FATAL_ERROR
        "expected stdout to match ${expected_stdout_regex}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

if(DEFINED forbidden_stdout_regex AND stdout_text MATCHES "${forbidden_stdout_regex}")
    message(FATAL_ERROR
        "expected stdout not to match ${forbidden_stdout_regex}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

set(combined_output "${stdout_text}\n${stderr_text}")
if(DEFINED expected_output_regex AND NOT combined_output MATCHES "${expected_output_regex}")
    message(FATAL_ERROR
        "expected output to match ${expected_output_regex}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

if(DEFINED forbidden_output_regex AND combined_output MATCHES "${forbidden_output_regex}")
    message(FATAL_ERROR
        "expected output not to match ${forbidden_output_regex}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()
