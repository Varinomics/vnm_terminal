if(NOT DEFINED timeline_path)
    message(FATAL_ERROR "timeline_path is required")
endif()

if(NOT DEFINED expected_interval_ms)
    set(expected_interval_ms "100")
endif()

if(NOT DEFINED expected_exit_code)
    set(expected_exit_code "0")
endif()

function(vnm_terminal_read_json_field out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error GET "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON field '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_expect_json_missing json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error TYPE "${json_text}" ${json_path})
    if(json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be absent")
    endif()
endfunction()

function(vnm_terminal_read_json_type out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error TYPE "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON field '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

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

file(REMOVE "${timeline_path}")
execute_process(
    COMMAND ${command_args}
    RESULT_VARIABLE actual_exit_code
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
)

if(NOT actual_exit_code STREQUAL "${expected_exit_code}")
    message(FATAL_ERROR
        "expected exit code ${expected_exit_code}, got ${actual_exit_code}\n"
        "stdout:\n${stdout_text}\n"
        "stderr:\n${stderr_text}")
endif()

if(NOT EXISTS "${timeline_path}")
    message(FATAL_ERROR "metrics timeline JSONL was not created: ${timeline_path}")
endif()

file(STRINGS "${timeline_path}" timeline_lines ENCODING UTF-8)
list(LENGTH timeline_lines timeline_line_count)
if(NOT timeline_line_count GREATER 0)
    message(FATAL_ERROR "metrics timeline JSONL has no samples: ${timeline_path}")
endif()

set(periodic_sample_count 0)
math(EXPR timeline_last_index "${timeline_line_count} - 1")
foreach(sample_index RANGE 0 ${timeline_last_index})
    list(GET timeline_lines ${sample_index} sample_text)

    vnm_terminal_read_json_field(schema
        "${sample_text}" "${timeline_path}" schema)
    if(NOT schema STREQUAL "vnm_terminal_metrics_timeline_sample_v1")
        message(FATAL_ERROR "unexpected timeline schema: ${schema}")
    endif()

    vnm_terminal_read_json_field(actual_sample_index
        "${sample_text}" "${timeline_path}" sample_index)
    if(NOT actual_sample_index STREQUAL "${sample_index}")
        message(FATAL_ERROR
            "timeline sample_index=${actual_sample_index}, expected ${sample_index}")
    endif()

    vnm_terminal_read_json_field(interval_ms
        "${sample_text}" "${timeline_path}" interval_ms)
    if(NOT interval_ms STREQUAL "${expected_interval_ms}")
        message(FATAL_ERROR
            "timeline interval_ms=${interval_ms}, expected ${expected_interval_ms}")
    endif()

    vnm_terminal_read_json_field(sample_elapsed_ms
        "${sample_text}" "${timeline_path}" elapsed_ms)
    vnm_terminal_read_json_field(runtime_elapsed_ms
        "${sample_text}" "${timeline_path}" runtime_metrics elapsed_ms)
    if(NOT sample_elapsed_ms STREQUAL "${runtime_elapsed_ms}")
        message(FATAL_ERROR
            "timeline sample elapsed_ms=${sample_elapsed_ms}, "
            "runtime_metrics.elapsed_ms=${runtime_elapsed_ms}")
    endif()

    vnm_terminal_read_json_field(runtime_schema
        "${sample_text}" "${timeline_path}" runtime_metrics schema)
    if(NOT runtime_schema STREQUAL "vnm_terminal_runtime_metrics_v2")
        message(FATAL_ERROR "unexpected runtime_metrics schema: ${runtime_schema}")
    endif()

    vnm_terminal_read_json_type(renderer_type
        "${sample_text}" "${timeline_path}" runtime_metrics renderer)
    if(NOT renderer_type STREQUAL "OBJECT")
        message(FATAL_ERROR "runtime_metrics.renderer should be an object")
    endif()

    vnm_terminal_read_json_field(sample_kind
        "${sample_text}" "${timeline_path}" kind)
    vnm_terminal_read_json_field(app_result_available
        "${sample_text}" "${timeline_path}" app_result_available)
    if(sample_index LESS timeline_last_index)
        if(NOT sample_kind STREQUAL "periodic")
            message(FATAL_ERROR
                "timeline sample ${sample_index} has kind='${sample_kind}', expected periodic")
        endif()

        if(app_result_available)
            message(FATAL_ERROR
                "timeline periodic sample ${sample_index} reports app_result_available=true")
        endif()

        vnm_terminal_expect_json_missing(
            "${sample_text}"
            "${timeline_path}"
            runtime_metrics app_result)
        math(EXPR periodic_sample_count "${periodic_sample_count} + 1")
    else()
        if(NOT sample_kind STREQUAL "final")
            message(FATAL_ERROR "last timeline sample should be final, got ${sample_kind}")
        endif()

        if(NOT app_result_available)
            message(FATAL_ERROR "final timeline sample reports app_result_available=false")
        endif()

        vnm_terminal_read_json_type(final_app_result_type
            "${sample_text}" "${timeline_path}" runtime_metrics app_result)
        if(NOT final_app_result_type STREQUAL "NUMBER")
            message(FATAL_ERROR
                "final runtime_metrics.app_result should be a number, got ${final_app_result_type}")
        endif()
    endif()
endforeach()

if(NOT periodic_sample_count GREATER 0)
    message(FATAL_ERROR "metrics timeline JSONL has no periodic samples: ${timeline_path}")
endif()
