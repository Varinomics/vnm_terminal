if(NOT DEFINED metrics_path)
    message(FATAL_ERROR "metrics_path is required")
endif()

if(NOT DEFINED expected_profile_text_requested)
    set(expected_profile_text_requested OFF)
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

function(vnm_terminal_read_json_length out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error LENGTH "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON object '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_expect_json_counter json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_field(counter_value "${json_text}" "${source_path}" ${json_path})
    if(NOT counter_value MATCHES "^[0-9]+$")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be an integer counter, "
            "got ${counter_value}")
    endif()
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

file(REMOVE "${metrics_path}")
if(DEFINED profile_text_path AND NOT profile_text_path STREQUAL "")
    file(REMOVE "${profile_text_path}")
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

if(NOT EXISTS "${metrics_path}")
    message(FATAL_ERROR "metrics JSON was not created: ${metrics_path}")
endif()

file(READ "${metrics_path}" metrics_text)

vnm_terminal_read_json_field(schema
    "${metrics_text}" "${metrics_path}" schema)
if(NOT schema STREQUAL "vnm_terminal_runtime_metrics_v2")
    message(FATAL_ERROR "unexpected metrics schema: ${schema}")
endif()

vnm_terminal_read_json_field(profile_text_requested
    "${metrics_text}" "${metrics_path}" profiling profile_text_requested)
if(expected_profile_text_requested)
    if(NOT profile_text_requested)
        message(FATAL_ERROR "profile_text_requested should be true for metrics smoke")
    endif()
else()
    if(profile_text_requested)
        message(FATAL_ERROR "profile_text_requested should be false for metrics smoke")
    endif()
endif()

vnm_terminal_read_json_field(profile_write_elapsed_ms
    "${metrics_text}" "${metrics_path}" profiling profile_write_elapsed_ms)
if(expected_profile_text_requested)
    if(NOT profile_write_elapsed_ms MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "profile_write_elapsed_ms should be an integer, got "
            "${profile_write_elapsed_ms}")
    endif()
    if(NOT DEFINED profile_text_path OR profile_text_path STREQUAL "")
        message(FATAL_ERROR "profile_text_path is required for profile smoke")
    endif()
    if(NOT EXISTS "${profile_text_path}")
        message(FATAL_ERROR "profile text was not written: ${profile_text_path}")
    endif()
    file(READ "${profile_text_path}" profile_text)
    foreach(profile_fragment IN ITEMS
        "dirty_rows"
        "enabled=true"
        "qsg_atlas"
        "renderer=atlas"
        "atlas_page_budget"
        "atlas_failed_inserts"
        "session_profile_stats")
        string(FIND "${profile_text}" "${profile_fragment}" profile_fragment_index)
        if(profile_fragment_index LESS 0)
            message(FATAL_ERROR
                "profile text is missing expected fragment: ${profile_fragment}")
        endif()
    endforeach()
else()
    if(NOT profile_write_elapsed_ms STREQUAL "0")
        message(FATAL_ERROR
            "profile_write_elapsed_ms should be 0, got ${profile_write_elapsed_ms}")
    endif()
endif()

if(DEFINED profile_text_path AND
    NOT profile_text_path STREQUAL "" AND
    NOT expected_profile_text_requested)
    message(FATAL_ERROR
        "profile_text_path was provided but profile text was not expected")
endif()

vnm_terminal_read_json_field(elapsed_ms_excludes_profile_write
    "${metrics_text}" "${metrics_path}" profiling elapsed_ms_excludes_profile_write)
if(NOT elapsed_ms_excludes_profile_write)
    message(FATAL_ERROR "elapsed_ms_excludes_profile_write should be true")
endif()

vnm_terminal_read_json_field(fps_elapsed_basis
    "${metrics_text}" "${metrics_path}" paint_frames_per_second_elapsed_basis)
if(NOT fps_elapsed_basis STREQUAL
    "app_exec_elapsed_ms_including_process_startup_excluding_profile_write")
    message(FATAL_ERROR "unexpected paint_frames_per_second_elapsed_basis")
endif()

vnm_terminal_read_json_field(font_size
    "${metrics_text}" "${metrics_path}" surface_geometry font_size)
if(NOT font_size MATCHES "^10(\\.0+)?$")
    message(FATAL_ERROR "metrics smoke expected font_size=10, got ${font_size}")
endif()

vnm_terminal_read_json_field(paint_completed_frames
    "${metrics_text}" "${metrics_path}" renderer paint_completed_frames)
if(NOT paint_completed_frames MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "metrics JSON reports no painted frames")
endif()

vnm_terminal_read_json_field(atlas_renderer
    "${metrics_text}" "${metrics_path}" qsg_atlas renderer)
if(NOT atlas_renderer STREQUAL "atlas")
    message(FATAL_ERROR "metrics JSON reports unexpected atlas renderer: ${atlas_renderer}")
endif()

vnm_terminal_read_json_field(atlas_capture_count
    "${metrics_text}" "${metrics_path}" qsg_atlas capture_count)
if(NOT atlas_capture_count MATCHES "^[0-9]+$")
    message(FATAL_ERROR "qsg_atlas.capture_count should be an integer counter")
endif()

vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload atlas_failed_inserts)

foreach(frame_counter IN ITEMS
    visible_rows
    dirty_rows
    packed_text_spans
    packed_text_cells
    packed_text_ascii_direct_cells
    packed_text_ascii_direct_bytes
    packed_text_utf8_cells
    packed_text_utf8_input_code_units
    packed_text_utf8_output_bytes)
    vnm_terminal_expect_json_counter(
        "${metrics_text}"
        "${metrics_path}"
        renderer frame ${frame_counter})
endforeach()
