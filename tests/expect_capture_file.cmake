if(NOT DEFINED capture_path)
    message(FATAL_ERROR "capture_path is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/vnm_terminal_cmake_script_helpers.cmake")

if(NOT DEFINED expected_capture_regex)
    message(FATAL_ERROR "expected_capture_regex is required")
endif()

if(NOT DEFINED expected_exit_code)
    set(expected_exit_code 0)
endif()

vnm_terminal_script_command_args(command_args)

file(REMOVE "${capture_path}")
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

if(NOT EXISTS "${capture_path}")
    message(FATAL_ERROR "capture file was not created: ${capture_path}")
endif()

file(READ "${capture_path}" capture_text)
if(NOT capture_text MATCHES "${expected_capture_regex}")
    message(FATAL_ERROR
        "expected capture to match ${expected_capture_regex}\n"
        "capture:\n${capture_text}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()
