if(NOT DEFINED capture_probe)
    message(FATAL_ERROR "capture_probe is required")
endif()

if(NOT DEFINED capture_base_path)
    message(FATAL_ERROR "capture_base_path is required")
endif()

if(NOT DEFINED expected_capture_text)
    message(FATAL_ERROR "expected_capture_text is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/vnm_terminal_cmake_script_helpers.cmake")

vnm_terminal_script_command_args(command_args)

execute_process(
    COMMAND "${capture_probe}" clear "${capture_base_path}"
    RESULT_VARIABLE clear_exit_code
    OUTPUT_VARIABLE clear_stdout
    ERROR_VARIABLE clear_stderr
)
if(NOT clear_exit_code STREQUAL "0")
    message(FATAL_ERROR
        "failed to clear backend output capture artifacts\n"
        "stdout:\n${clear_stdout}\n"
        "stderr:\n${clear_stderr}")
endif()

execute_process(
    COMMAND ${command_args}
    RESULT_VARIABLE actual_exit_code
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
)
if(NOT actual_exit_code STREQUAL "0")
    message(FATAL_ERROR
        "expected exit code 0, got ${actual_exit_code}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

execute_process(
    COMMAND
        "${capture_probe}" check
        "${capture_base_path}"
        "${expected_capture_text}"
    RESULT_VARIABLE check_exit_code
    OUTPUT_VARIABLE check_stdout
    ERROR_VARIABLE check_stderr
)
if(NOT check_exit_code STREQUAL "0")
    message(FATAL_ERROR
        "backend output capture validation failed\n"
        "stdout:\n${check_stdout}\n"
        "stderr:\n${check_stderr}\n"
        "app stdout:\n${stdout_text}\n"
        "app stderr:\n${stderr_text}")
endif()
