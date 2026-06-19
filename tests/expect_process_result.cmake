if(NOT DEFINED expected_exit_code)
    message(FATAL_ERROR "expected_exit_code is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/vnm_terminal_cmake_script_helpers.cmake")

if(NOT DEFINED expected_output_regex)
    message(FATAL_ERROR "expected_output_regex is required")
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

set(combined_output "${stdout_text}\n${stderr_text}")
if(NOT combined_output MATCHES "${expected_output_regex}")
    message(FATAL_ERROR
        "expected output to match ${expected_output_regex}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()
