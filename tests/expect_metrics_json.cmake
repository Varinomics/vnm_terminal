if(NOT DEFINED metrics_path)
    message(FATAL_ERROR "metrics_path is required")
endif()

if(NOT DEFINED expected_stage42_toggle_count)
    set(expected_stage42_toggle_count 16)
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
if(NOT schema STREQUAL "vnm_terminal_runtime_metrics_v1")
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

vnm_terminal_read_json_length(stage42_toggle_count
    "${metrics_text}" "${metrics_path}" stage42_toggles)
if(NOT stage42_toggle_count STREQUAL "${expected_stage42_toggle_count}")
    message(FATAL_ERROR
        "expected ${expected_stage42_toggle_count} Stage 4.2 toggles, got "
        "${stage42_toggle_count}")
endif()

set(expected_stage42_toggles
    model_ascii_direct_print
    model_ascii_skip_simple_cell_clear
    qsg_ascii_resource_prefilter
    qsg_cached_internal_text_node
    qsg_descriptor_reuse_frame_key_independent
    qsg_group_descriptor_eligibility
    qsg_monotonic_dirty_probe
    qsg_row_slot_ordered_lookup
    qsg_text_leaf_content_reuse
    qsg_text_makeup_single_char_fast_path
    qsg_text_resource_descriptor_direct_compare
    qsg_trusted_ascii_glyph_batching
    qsg_trusted_ascii_unchecked_glyphs
    render_cell_row_cache
    render_frame_sorted_row_sort_prefilter
    snapshot_inline_hyperlink_ids
)

foreach(toggle_name IN LISTS expected_stage42_toggles)
    vnm_terminal_read_json_field(toggle_enabled
        "${metrics_text}" "${metrics_path}" stage42_toggles ${toggle_name} enabled)
    vnm_terminal_read_json_field(toggle_environment
        "${metrics_text}" "${metrics_path}" stage42_toggles ${toggle_name} environment)
    if(toggle_environment STREQUAL "")
        message(FATAL_ERROR "Stage 4.2 toggle ${toggle_name} has no environment name")
    endif()
endforeach()

vnm_terminal_read_json_field(default_toggle_enabled
    "${metrics_text}" "${metrics_path}" stage42_toggles model_ascii_direct_print enabled)
if(NOT default_toggle_enabled)
    message(FATAL_ERROR "model_ascii_direct_print should default to enabled")
endif()

vnm_terminal_read_json_field(default_toggle_set
    "${metrics_text}" "${metrics_path}" stage42_toggles model_ascii_direct_print set)
if(default_toggle_set)
    message(FATAL_ERROR "model_ascii_direct_print should be unset by this smoke test")
endif()

vnm_terminal_read_json_type(default_toggle_raw_value_type
    "${metrics_text}" "${metrics_path}" stage42_toggles model_ascii_direct_print raw_value)
if(NOT default_toggle_raw_value_type STREQUAL "NULL")
    message(FATAL_ERROR
        "model_ascii_direct_print raw_value should be null, got "
        "${default_toggle_raw_value_type}")
endif()

vnm_terminal_read_json_field(row_cache_enabled
    "${metrics_text}" "${metrics_path}" stage42_toggles render_cell_row_cache enabled)
if(row_cache_enabled)
    message(FATAL_ERROR "render_cell_row_cache should be disabled by this smoke test")
endif()

vnm_terminal_read_json_field(row_cache_set
    "${metrics_text}" "${metrics_path}" stage42_toggles render_cell_row_cache set)
if(NOT row_cache_set)
    message(FATAL_ERROR "render_cell_row_cache environment should be marked set")
endif()

vnm_terminal_read_json_field(row_cache_raw_value
    "${metrics_text}" "${metrics_path}" stage42_toggles render_cell_row_cache raw_value)
if(NOT row_cache_raw_value STREQUAL "0")
    message(FATAL_ERROR
        "render_cell_row_cache raw_value should be 0, got ${row_cache_raw_value}")
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
