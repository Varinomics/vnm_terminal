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

function(vnm_terminal_expect_json_boolean json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_type(field_type "${json_text}" "${source_path}" ${json_path})
    if(NOT field_type STREQUAL "BOOLEAN")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be a boolean, "
            "got ${field_type}")
    endif()
endfunction()

function(vnm_terminal_expect_json_number json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_type(field_type "${json_text}" "${source_path}" ${json_path})
    if(NOT field_type STREQUAL "NUMBER")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be a number, "
            "got ${field_type}")
    endif()
endfunction()

function(vnm_terminal_expect_renderer_frame_evidence json_text source_path)
    vnm_terminal_read_json_field(evidence_counter_path
        "${json_text}" "${source_path}" renderer_frame_evidence counter_path)
    vnm_terminal_read_json_field(evidence_frame_count
        "${json_text}" "${source_path}" renderer_frame_evidence frame_count)
    vnm_terminal_read_json_field(evidence_frames_per_second
        "${json_text}" "${source_path}" renderer_frame_evidence frames_per_second)
    vnm_terminal_read_json_field(evidence_elapsed_basis
        "${json_text}" "${source_path}" renderer_frame_evidence elapsed_basis)
    vnm_terminal_expect_json_counter(
        "${json_text}"
        "${source_path}"
        renderer_frame_evidence frame_count)
    vnm_terminal_expect_json_number(
        "${json_text}"
        "${source_path}"
        renderer_frame_evidence frames_per_second)

    if(NOT evidence_elapsed_basis STREQUAL
        "app_exec_elapsed_ms_including_process_startup_excluding_profile_write")
        message(FATAL_ERROR "unexpected renderer_frame_evidence.elapsed_basis")
    endif()

    if(NOT evidence_frames_per_second GREATER 0)
        message(FATAL_ERROR
            "renderer_frame_evidence.frames_per_second should be positive, "
            "got ${evidence_frames_per_second}")
    endif()

    if(evidence_counter_path STREQUAL "renderer.paint_completed_frames")
        vnm_terminal_read_json_field(producer_frame_count
            "${json_text}" "${source_path}" renderer paint_completed_frames)
    elseif(evidence_counter_path STREQUAL "qsg_atlas.render_count")
        vnm_terminal_read_json_field(producer_frame_count
            "${json_text}" "${source_path}" qsg_atlas render_count)
    else()
        message(FATAL_ERROR
            "unexpected renderer_frame_evidence.counter_path: ${evidence_counter_path}")
    endif()

    if(NOT evidence_frame_count STREQUAL producer_frame_count)
        message(FATAL_ERROR
            "renderer_frame_evidence.frame_count=${evidence_frame_count} "
            "does not match ${evidence_counter_path}=${producer_frame_count}")
    endif()

    if(NOT evidence_frame_count MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "renderer_frame_evidence.frame_count should be positive, "
            "got ${evidence_frame_count}")
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
        "producer"
        "warm_lazy"
        "warm_completed"
        "warm_seed_strings"
        "warm_shaped_glyph_records"
        "warm_covered_glyph_records"
        "warm_environment_skipped_glyph_records"
        "warm_failed_glyph_records"
        "warm_failed_inserts"
        "lazy_insert_attempts"
        "lazy_failed_inserts"
        "incomplete_frames"
        "shape_cache_hits"
        "shaped_runs_reused"
        "placement"
        "misses"
        "coverage"
        "coverage_texture_created"
        "coverage_upload_recorded"
        "sampler_mode"
        "atlas_page_budget"
        "atlas_failed_inserts"
        "atlas_page_pressure"
        "render"
        "capabilities"
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
vnm_terminal_read_json_field(atlas_prepare_count
    "${metrics_text}" "${metrics_path}" qsg_atlas prepare_count)
if(NOT atlas_prepare_count MATCHES "^[0-9]+$")
    message(FATAL_ERROR "qsg_atlas.prepare_count should be an integer counter")
endif()
vnm_terminal_read_json_field(atlas_render_count
    "${metrics_text}" "${metrics_path}" qsg_atlas render_count)
if(NOT atlas_render_count MATCHES "^[0-9]+$")
    message(FATAL_ERROR "qsg_atlas.render_count should be an integer counter")
endif()

vnm_terminal_read_json_field(paint_completed_frames
    "${metrics_text}" "${metrics_path}" renderer paint_completed_frames)
if(NOT paint_completed_frames MATCHES "^[0-9]+$")
    message(FATAL_ERROR "renderer.paint_completed_frames should be an integer counter")
endif()
if(paint_completed_frames STREQUAL "0" AND atlas_render_count STREQUAL "0")
    message(FATAL_ERROR
        "metrics JSON reports no painted frames and no atlas render frames")
endif()
vnm_terminal_expect_renderer_frame_evidence("${metrics_text}" "${metrics_path}")
vnm_terminal_expect_json_number(
    "${metrics_text}"
    "${metrics_path}"
    startup first_output_elapsed_ms)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    startup output_seen)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    startup visible_first_frame_completed)
vnm_terminal_read_json_field(
    startup_visible_frame_counter_path
    "${metrics_text}"
    "${metrics_path}"
    startup visible_first_frame_counter_path)
if(NOT startup_visible_frame_counter_path MATCHES
    "^(renderer\\.paint_completed_frames|qsg_atlas\\.render_count)$")
    message(FATAL_ERROR
        "unexpected startup.visible_first_frame_counter_path: "
        "${startup_visible_frame_counter_path}")
endif()

vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload atlas_failed_inserts)
vnm_terminal_read_json_field(shaped_text_runs
    "${metrics_text}" "${metrics_path}" qsg_atlas buffer_upload shaped_text_runs)
vnm_terminal_read_json_field(shaped_glyph_records
    "${metrics_text}" "${metrics_path}" qsg_atlas buffer_upload shaped_glyph_records)
vnm_terminal_read_json_field(shaped_missing_string_indexes
    "${metrics_text}" "${metrics_path}"
    qsg_atlas buffer_upload shaped_missing_string_indexes)
vnm_terminal_read_json_field(shaped_invalid_string_indexes
    "${metrics_text}" "${metrics_path}"
    qsg_atlas buffer_upload shaped_invalid_string_indexes)
vnm_terminal_read_json_field(atlas_glyph_buffer_instances
    "${metrics_text}" "${metrics_path}" qsg_atlas buffer_upload glyph_buffer_instances)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload shaped_text_runs)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload shaped_glyph_records)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload shaped_missing_string_indexes)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload shaped_invalid_string_indexes)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload glyph_buffer_instances)
if(NOT shaped_missing_string_indexes STREQUAL "0")
    message(FATAL_ERROR
        "qsg_atlas.buffer_upload.shaped_missing_string_indexes should be zero, "
        "got ${shaped_missing_string_indexes}")
endif()
if(NOT shaped_invalid_string_indexes STREQUAL "0")
    message(FATAL_ERROR
        "qsg_atlas.buffer_upload.shaped_invalid_string_indexes should be zero, "
        "got ${shaped_invalid_string_indexes}")
endif()
foreach(producer_counter IN ITEMS
    text_runs_considered
    text_runs_empty
    shape_cache_lookups
    shape_cache_hits
    shape_cache_misses
    shape_cache_inserts
    shape_cache_pruned
    shape_cache_entries
    shaped_runs_built
    shaped_runs_reused
    shaped_glyph_records_built
    shaped_glyph_records_reused
    presentation_run_scans
    presentation_source_scans
    presentation_fast_text_runs
    presentation_emoji_runs
    slot_resolutions_built
    slot_resolutions_reused
    simple_path_attempts
    simple_path_used
    simple_path_fallbacks)
    vnm_terminal_expect_json_counter(
        "${metrics_text}"
        "${metrics_path}"
        qsg_atlas producer ${producer_counter})
endforeach()

vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas warm_lazy warm_completed)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas warm_lazy warm_page_pressure)
vnm_terminal_expect_json_number(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas warm_lazy warm_elapsed_ms)
vnm_terminal_expect_json_number(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas warm_lazy lazy_elapsed_ms)

foreach(warm_lazy_counter IN ITEMS
    warm_epoch
    warm_seed_strings
    warm_shaped_glyph_records
    warm_covered_glyph_records
    warm_skipped_glyph_records
    warm_environment_skipped_glyph_records
    warm_failed_glyph_records
    warm_missing_string_indexes
    warm_invalid_string_indexes
    warm_unsupported_images
    warm_cache_hits
    warm_insert_attempts
    warm_inserts
    warm_failed_inserts
    lazy_insert_attempts
    lazy_inserts
    lazy_failed_inserts
    lazy_max_insert_us
    lazy_frames
    incomplete_frames)
    vnm_terminal_expect_json_counter(
        "${metrics_text}"
        "${metrics_path}"
        qsg_atlas warm_lazy ${warm_lazy_counter})
endforeach()

vnm_terminal_read_json_field(warm_completed
    "${metrics_text}" "${metrics_path}" qsg_atlas warm_lazy warm_completed)
vnm_terminal_read_json_field(warm_seed_strings
    "${metrics_text}" "${metrics_path}" qsg_atlas warm_lazy warm_seed_strings)
vnm_terminal_read_json_field(warm_shaped_glyph_records
    "${metrics_text}" "${metrics_path}" qsg_atlas warm_lazy warm_shaped_glyph_records)
vnm_terminal_read_json_field(warm_covered_glyph_records
    "${metrics_text}" "${metrics_path}" qsg_atlas warm_lazy warm_covered_glyph_records)
if(atlas_prepare_count MATCHES "^[1-9][0-9]*$")
    if(NOT warm_completed)
        message(FATAL_ERROR
            "qsg_atlas.warm_lazy.warm_completed should be true when "
            "qsg_atlas.prepare_count is positive")
    endif()
    if(NOT warm_seed_strings MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "qsg_atlas.warm_lazy.warm_seed_strings should be positive when "
            "qsg_atlas.prepare_count is positive")
    endif()
    if(NOT warm_shaped_glyph_records MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "qsg_atlas.warm_lazy.warm_shaped_glyph_records should be positive "
            "when qsg_atlas.prepare_count is positive")
    endif()
    if(NOT warm_covered_glyph_records MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "qsg_atlas.warm_lazy.warm_covered_glyph_records should be positive "
            "when qsg_atlas.prepare_count is positive")
    endif()
endif()
foreach(zero_counter IN ITEMS
    warm_failed_glyph_records
    warm_missing_string_indexes
    warm_invalid_string_indexes
    warm_unsupported_images
    warm_failed_inserts
    lazy_failed_inserts
    incomplete_frames)
    vnm_terminal_read_json_field(zero_value
        "${metrics_text}" "${metrics_path}" qsg_atlas warm_lazy ${zero_counter})
    if(NOT zero_value STREQUAL "0")
        message(FATAL_ERROR
            "qsg_atlas.warm_lazy.${zero_counter} should be zero, got ${zero_value}")
    endif()
endforeach()

vnm_terminal_read_json_field(producer_text_runs_considered
    "${metrics_text}" "${metrics_path}" qsg_atlas producer text_runs_considered)
vnm_terminal_read_json_field(producer_shaped_runs_built
    "${metrics_text}" "${metrics_path}" qsg_atlas producer shaped_runs_built)
vnm_terminal_read_json_field(producer_shaped_runs_reused
    "${metrics_text}" "${metrics_path}" qsg_atlas producer shaped_runs_reused)
vnm_terminal_read_json_field(producer_shaped_glyphs_built
    "${metrics_text}" "${metrics_path}" qsg_atlas producer shaped_glyph_records_built)
vnm_terminal_read_json_field(producer_shaped_glyphs_reused
    "${metrics_text}" "${metrics_path}" qsg_atlas producer shaped_glyph_records_reused)
if(atlas_glyph_buffer_instances MATCHES "^[1-9][0-9]*$" AND
    NOT producer_text_runs_considered MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "qsg_atlas.producer.text_runs_considered should be positive "
        "when glyph buffer instances are positive")
endif()
if(atlas_glyph_buffer_instances MATCHES "^[1-9][0-9]*$")
    math(EXPR producer_shaped_runs_total
        "${producer_shaped_runs_built} + ${producer_shaped_runs_reused}")
    math(EXPR producer_shaped_glyphs_total
        "${producer_shaped_glyphs_built} + ${producer_shaped_glyphs_reused}")
    if(NOT producer_shaped_runs_total GREATER 0)
        message(FATAL_ERROR
            "qsg_atlas.producer shaped run total should be positive "
            "when glyph buffer instances are positive")
    endif()
    if(NOT producer_shaped_glyphs_total GREATER 0)
        message(FATAL_ERROR
            "qsg_atlas.producer shaped glyph total should be positive "
            "when glyph buffer instances are positive")
    endif()
endif()
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas buffer_upload atlas_page_pressure)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas coverage_texture_created)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas coverage_upload_recorded)

foreach(coverage_counter IN ITEMS
    grayscale_masks
    lcd_rgb_masks
    lcd_bgr_masks
    color_images
    ambiguous_images
    unsupported_images
    missed_images)
    vnm_terminal_expect_json_counter(
        "${metrics_text}"
        "${metrics_path}"
        qsg_atlas coverage ${coverage_counter})
endforeach()

vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas snapped_origin_failures)
vnm_terminal_expect_json_counter(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas max_glyph_instance_page)

foreach(miss_counter IN ITEMS
    glyph_missed_instances
    glyph_coverage_failures
    glyph_atlas_insert_failures)
    vnm_terminal_expect_json_counter(
        "${metrics_text}"
        "${metrics_path}"
        qsg_atlas ${miss_counter})
endforeach()

vnm_terminal_read_json_type(
    first_glyph_miss_type
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas first_glyph_miss)
if(NOT first_glyph_miss_type STREQUAL "OBJECT")
    message(FATAL_ERROR
        "qsg_atlas.first_glyph_miss should be a JSON object, "
        "got ${first_glyph_miss_type}")
endif()
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas first_glyph_miss valid)

vnm_terminal_read_json_field(
    atlas_sampler_mode
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas sampler_mode)
if(NOT atlas_sampler_mode MATCHES "^(unknown|nearest|linear)$")
    message(FATAL_ERROR "unexpected qsg_atlas.sampler_mode: ${atlas_sampler_mode}")
endif()
if(atlas_sampler_mode STREQUAL "linear")
    message(FATAL_ERROR "qsg_atlas.sampler_mode should not be linear")
endif()
if(atlas_glyph_buffer_instances MATCHES "^[1-9][0-9]*$" AND
    NOT atlas_sampler_mode STREQUAL "nearest")
    message(FATAL_ERROR
        "qsg_atlas.sampler_mode should be nearest when glyph buffer "
        "instances are positive")
endif()

vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas capabilities glyph_shader_package_available)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas capabilities dual_source_probe_shader_package_available)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas capabilities dual_source_blend_factors_available)
vnm_terminal_expect_json_boolean(
    "${metrics_text}"
    "${metrics_path}"
    qsg_atlas capabilities dual_source_blend_factors_runtime_probe)

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
