if(NOT DEFINED expected_exit_code)
    message(FATAL_ERROR "expected_exit_code is required")
endif()

set(stderr_must_be_empty OFF)
if(DEFINED require_empty_stderr)
    set(stderr_must_be_empty "${require_empty_stderr}")
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
