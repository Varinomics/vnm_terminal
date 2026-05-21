if(NOT DEFINED capture_path)
    message(FATAL_ERROR "capture_path is required")
endif()

if(NOT DEFINED expected_capture_regex)
    message(FATAL_ERROR "expected_capture_regex is required")
endif()

if(NOT DEFINED expected_exit_code)
    set(expected_exit_code 0)
endif()

set(separator_index -1)
math(EXPR last_index "${CMAKE_ARGC} - 1")
foreach(index RANGE 0 ${last_index})
    if(separator_index LESS 0 AND "${CMAKE_ARGV${index}}" STREQUAL "--")
        set(separator_index ${index})
    endif()
endforeach()

if(separator_index LESS 0)
    message(FATAL_ERROR "expected process command after --")
endif()

math(EXPR command_start "${separator_index} + 1")
if(command_start GREATER last_index)
    message(FATAL_ERROR "expected process command after --")
endif()

set(command_args)
foreach(index RANGE ${command_start} ${last_index})
    set(command_arg "${CMAKE_ARGV${index}}")
    string(REPLACE ";" "\\;" command_arg "${command_arg}")
    list(APPEND command_args "${command_arg}")
endforeach()

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
