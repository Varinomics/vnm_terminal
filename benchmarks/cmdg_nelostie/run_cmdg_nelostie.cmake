include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/vnm_terminal_cmake_script_helpers.cmake")

function(vnm_terminal_expect_presentation_counter_metadata json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_field(presentation_counter_source
        "${json_text}" "${source_path}" ${json_path} primary_counter_source)
    vnm_terminal_read_json_field(presentation_counter_semantics
        "${json_text}" "${source_path}" ${json_path} primary_counter_semantics)
    vnm_terminal_read_json_field(presentation_scanout_verified
        "${json_text}" "${source_path}" ${json_path} scanout_verified)

    if(NOT presentation_counter_source STREQUAL "QQuickWindow::frameSwapped")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "terminal metrics JSON reports unexpected "
            "${json_path_text}.primary_counter_source: "
            "${presentation_counter_source}")
    endif()

    if(NOT presentation_counter_semantics STREQUAL "qt_frame_swapped_proxy")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "terminal metrics JSON reports unexpected "
            "${json_path_text}.primary_counter_semantics: "
            "${presentation_counter_semantics}")
    endif()

    if(presentation_scanout_verified)
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "terminal metrics JSON reports ${json_path_text}.scanout_verified=true")
    endif()
endfunction()

function(vnm_terminal_expect_renderer_frame_evidence json_text source_path)
    vnm_terminal_read_json_field(evidence_counter_path
        "${json_text}" "${source_path}" renderer_frame_evidence counter_path)
    vnm_terminal_read_json_field(evidence_frame_count
        "${json_text}" "${source_path}" renderer_frame_evidence frame_count)
    vnm_terminal_read_json_field(evidence_frames_per_second
        "${json_text}" "${source_path}" renderer_frame_evidence frames_per_second)

    if(evidence_counter_path STREQUAL "renderer.paint_completed_frames")
        vnm_terminal_read_json_field(producer_frame_count
            "${json_text}" "${source_path}" renderer paint_completed_frames)
    elseif(evidence_counter_path STREQUAL "qsg_atlas.render_count")
        vnm_terminal_read_json_field(producer_frame_count
            "${json_text}" "${source_path}" qsg_atlas render_count)
    else()
        message(FATAL_ERROR
            "terminal metrics JSON reports unexpected renderer_frame_evidence.counter_path: "
            "${evidence_counter_path}")
    endif()

    if(NOT evidence_frame_count STREQUAL producer_frame_count)
        message(FATAL_ERROR
            "terminal metrics JSON reports renderer_frame_evidence.frame_count="
            "${evidence_frame_count}, expected "
            "${evidence_counter_path}=${producer_frame_count}")
    endif()

    if(NOT evidence_frame_count MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "terminal metrics JSON reports no renderer frame evidence")
    endif()

    if(NOT evidence_frames_per_second GREATER 0)
        message(FATAL_ERROR
            "terminal metrics JSON reports non-positive renderer frame evidence FPS: "
            "${evidence_frames_per_second}")
    endif()

    vnm_terminal_read_json_field(startup_first_output_elapsed_ms
        "${json_text}" "${source_path}" startup first_output_elapsed_ms)
    vnm_terminal_read_json_field(visible_first_frame_completed
        "${json_text}" "${source_path}" startup visible_first_frame_completed)

    if(NOT startup_first_output_elapsed_ms MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "terminal metrics JSON reports no startup first-output latency")
    endif()

    if(NOT visible_first_frame_completed)
        message(FATAL_ERROR
            "terminal metrics JSON reports no visible first-frame completion")
    endif()
endfunction()

function(vnm_terminal_expect_presentation_frame_evidence json_text source_path)
    vnm_terminal_read_json_field(presentation_counter_path
        "${json_text}" "${source_path}" presentation primary_counter_path)
    vnm_terminal_read_json_field(presentation_frame_count
        "${json_text}" "${source_path}" presentation primary_frame_count)
    vnm_terminal_read_json_field(presentation_frames_per_second
        "${json_text}" "${source_path}" presentation primary_frames_per_second)
    vnm_terminal_read_json_field(frame_swapped_count
        "${json_text}" "${source_path}" presentation frameSwapped count)

    if(NOT presentation_counter_path STREQUAL "presentation.frameSwapped.count")
        message(FATAL_ERROR
            "terminal metrics JSON reports unexpected presentation.primary_counter_path: "
            "${presentation_counter_path}")
    endif()
    vnm_terminal_expect_presentation_counter_metadata(
        "${json_text}"
        "${source_path}"
        presentation)

    if(NOT presentation_frame_count STREQUAL frame_swapped_count)
        message(FATAL_ERROR
            "terminal metrics JSON reports presentation.primary_frame_count="
            "${presentation_frame_count}, expected presentation.frameSwapped.count="
            "${frame_swapped_count}")
    endif()

    if(NOT presentation_frame_count MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "terminal metrics JSON reports no Qt frameSwapped proxy evidence")
    endif()

    if(NOT presentation_frames_per_second GREATER 0)
        message(FATAL_ERROR
            "terminal metrics JSON reports non-positive presentation proxy FPS: "
            "${presentation_frames_per_second}")
    endif()
endfunction()

if(NOT DEFINED scene OR scene STREQUAL "")
    set(scene "AssemblyWinter2025")
endif()

if(NOT scene MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
    message(FATAL_ERROR "CMDG scene name is invalid: ${scene}")
endif()

if(NOT DEFINED run_id OR run_id STREQUAL "")
    set(run_id "nelostie")
    set(use_legacy_output_names ON)
else()
    set(use_legacy_output_names OFF)
endif()

if(NOT run_id MATCHES "^[A-Za-z0-9_]+$")
    message(FATAL_ERROR "CMDG benchmark run id is invalid: ${run_id}")
endif()

if(NOT DEFINED build_only)
    set(build_only OFF)
endif()

if(NOT build_only)
    if(NOT DEFINED output_dir OR output_dir STREQUAL "")
        message(FATAL_ERROR "CMDG benchmark output_dir was not provided")
    endif()

    if(NOT DEFINED frame_limit OR
        NOT frame_limit MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "CMDG benchmark frame_limit must be a positive integer")
    endif()

    if(NOT DEFINED metrics_interval_ms OR metrics_interval_ms STREQUAL "")
        set(metrics_interval_ms "5000")
    endif()

    vnm_terminal_validate_positive_int32(
        "CMDG benchmark metrics_interval_ms"
        "${metrics_interval_ms}")

    if(NOT DEFINED benchmark_min_windows OR benchmark_min_windows STREQUAL "")
        set(benchmark_min_windows "3")
    endif()

    vnm_terminal_validate_positive_int32(
        "CMDG benchmark benchmark_min_windows"
        "${benchmark_min_windows}")

    if(NOT DEFINED timeout_seconds OR
        NOT timeout_seconds MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "CMDG benchmark timeout_seconds must be a positive integer")
    endif()
endif()

if(NOT DEFINED font_size OR font_size STREQUAL "")
    set(font_size "10")
endif()

if(NOT font_size MATCHES "^[0-9]+(\\.[0-9]+)?$" OR
    font_size MATCHES "^0+(\\.0+)?$")
    message(FATAL_ERROR "CMDG benchmark font_size must be a positive pixel size")
endif()

if(NOT DEFINED hide_cursor)
    set(hide_cursor OFF)
endif()

if(NOT DEFINED build_lock_path OR build_lock_path STREQUAL "")
    set(build_lock_path "${output_dir}/cmdg_build.lock")
endif()

if(NOT build_only AND NOT EXISTS "${terminal_exe}")
    message(FATAL_ERROR "vnm_terminal executable does not exist: ${terminal_exe}")
endif()

if(build_cmdg)
    if(NOT IS_DIRECTORY "${cmdg_source_dir}")
        message(FATAL_ERROR "CMDG source directory does not exist: ${cmdg_source_dir}")
    endif()

    get_filename_component(build_lock_dir "${build_lock_path}" DIRECTORY)
    if(build_lock_dir)
        file(MAKE_DIRECTORY "${build_lock_dir}")
    endif()
    file(LOCK "${build_lock_path}" GUARD PROCESS TIMEOUT 600)

    execute_process(
        COMMAND dotnet build "${cmdg_source_dir}/CMDG/CMDG.csproj" -c Release
        WORKING_DIRECTORY "${cmdg_source_dir}"
        RESULT_VARIABLE cmdg_build_result
    )

    if(NOT cmdg_build_result EQUAL 0)
        message(FATAL_ERROR "CMDG Release build failed with exit code ${cmdg_build_result}")
    endif()
endif()

if(NOT EXISTS "${cmdg_exe}")
    message(FATAL_ERROR "CMDG executable does not exist: ${cmdg_exe}")
endif()

if(build_only)
    message(STATUS "CMDG build ready: ${cmdg_exe}")
    return()
endif()

if(NOT IS_DIRECTORY "${cmdg_working_dir}")
    message(FATAL_ERROR "CMDG working directory does not exist: ${cmdg_working_dir}")
endif()

file(MAKE_DIRECTORY "${output_dir}")
if(use_legacy_output_names)
    set(terminal_metrics "${output_dir}/vnm_terminal_cmdg_nelostie_terminal_metrics.json")
    set(terminal_timeline "${output_dir}/vnm_terminal_cmdg_nelostie_terminal_timeline.jsonl")
    set(cmdg_metrics "${output_dir}/vnm_terminal_cmdg_nelostie_cmdg_metrics.json")
else()
    set(terminal_metrics "${output_dir}/vnm_terminal_cmdg_${run_id}_terminal_metrics.json")
    set(terminal_timeline "${output_dir}/vnm_terminal_cmdg_${run_id}_terminal_timeline.jsonl")
    set(cmdg_metrics "${output_dir}/vnm_terminal_cmdg_${run_id}_cmdg_metrics.json")
endif()
file(REMOVE "${terminal_metrics}" "${terminal_timeline}" "${cmdg_metrics}")
if(profile_text)
    file(REMOVE "${profile_text}")
endif()

math(EXPR timeout_ms "${timeout_seconds} * 1000")

set(terminal_arguments
    "${terminal_exe}"
    "--metrics-json" "${terminal_metrics}"
    "--metrics-timeline-jsonl" "${terminal_timeline}"
    "--metrics-timeline-interval-ms" "${metrics_interval_ms}"
    "--font-size" "${font_size}"
    "--window-size" "${window_size}"
    "--timeout-ms" "${timeout_ms}"
    "--require-output"
    "--cwd" "${cmdg_working_dir}")

if(profile_text)
    list(APPEND terminal_arguments "--profile-text" "${profile_text}")
endif()

list(APPEND terminal_arguments
    "--"
    "${cmdg_exe}")

if(WIN32)
    set(ENV{PATH} "${qt_bin_dir};$ENV{PATH}")
elseif(APPLE)
    set(ENV{DYLD_LIBRARY_PATH} "${qt_bin_dir}:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(ENV{LD_LIBRARY_PATH} "${qt_bin_dir}:$ENV{LD_LIBRARY_PATH}")
endif()

set(ENV{CMDG_BENCHMARK} "1")
set(ENV{CMDG_SCENE} "${scene}")
set(ENV{CMDG_DISABLE_AUDIO} "1")
set(ENV{CMDG_ADJUST_SCREEN} "0")
set(ENV{CMDG_SPLASH_SCREEN} "0")
set(ENV{CMDG_BENCHMARK_FRAME_LIMIT} "${frame_limit}")
set(ENV{CMDG_BENCHMARK_METRICS} "${cmdg_metrics}")
set(ENV{CMDG_BENCHMARK_WINDOW_MS} "${metrics_interval_ms}")
set(ENV{CMDG_BENCHMARK_HIDE_CURSOR} "${hide_cursor}")

if(offscreen)
    set(ENV{QT_QPA_PLATFORM} "offscreen")
    set(ENV{QT_QPA_PLATFORM_PLUGIN_PATH} "${offscreen_plugin_dir}")
    set(ENV{QT_SCALE_FACTOR} "1")
    set(ENV{QT_SCREEN_SCALE_FACTORS} "")
    set(ENV{QT_AUTO_SCREEN_SCALE_FACTOR} "0")
    set(ENV{QT_DEVICE_PIXEL_RATIO} "")
    set(ENV{QT_SCALE_FACTOR_ROUNDING_POLICY} "PassThrough")
endif()

execute_process(
    COMMAND ${terminal_arguments}
    WORKING_DIRECTORY "${cmdg_working_dir}"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "CMDG benchmark scene ${scene} (${run_id}) failed with exit code ${result}")
endif()

if(NOT EXISTS "${terminal_metrics}")
    message(FATAL_ERROR "terminal metrics JSON was not written: ${terminal_metrics}")
endif()

if(NOT EXISTS "${terminal_timeline}")
    message(FATAL_ERROR "terminal metrics timeline JSONL was not written: ${terminal_timeline}")
endif()

if(NOT EXISTS "${cmdg_metrics}")
    message(FATAL_ERROR "CMDG metrics JSON was not written: ${cmdg_metrics}")
endif()

if(profile_text AND NOT EXISTS "${profile_text}")
    message(FATAL_ERROR "terminal profile text was not written: ${profile_text}")
endif()

file(READ "${terminal_metrics}" terminal_metrics_text)
file(STRINGS "${terminal_timeline}" terminal_timeline_lines ENCODING UTF-8)
file(READ "${cmdg_metrics}" cmdg_metrics_text)

list(LENGTH terminal_timeline_lines terminal_timeline_line_count)
if(NOT terminal_timeline_line_count GREATER 0)
    message(FATAL_ERROR
        "terminal metrics timeline JSONL has no samples: ${terminal_timeline}")
endif()

vnm_terminal_read_json_field(terminal_backend_error_count
    "${terminal_metrics_text}" "${terminal_metrics}" backend_error_count)
vnm_terminal_read_json_field(terminal_timeout_expired
    "${terminal_metrics_text}" "${terminal_metrics}" timeout_expired)

if(NOT terminal_backend_error_count STREQUAL "0")
    message(FATAL_ERROR
        "terminal metrics JSON reports backend_error_count=${terminal_backend_error_count}")
endif()

if(terminal_timeout_expired)
    message(FATAL_ERROR "terminal metrics JSON reports a timeout")
endif()

vnm_terminal_expect_renderer_frame_evidence("${terminal_metrics_text}" "${terminal_metrics}")
vnm_terminal_expect_presentation_frame_evidence("${terminal_metrics_text}" "${terminal_metrics}")

set(terminal_timeline_periodic_count 0)
math(EXPR terminal_timeline_last_index "${terminal_timeline_line_count} - 1")
foreach(terminal_timeline_index RANGE 0 ${terminal_timeline_last_index})
    list(GET terminal_timeline_lines ${terminal_timeline_index} terminal_timeline_sample_text)

    vnm_terminal_read_json_field(terminal_timeline_schema
        "${terminal_timeline_sample_text}" "${terminal_timeline}" schema)
    vnm_terminal_read_json_field(terminal_timeline_sample_index
        "${terminal_timeline_sample_text}" "${terminal_timeline}" sample_index)
    vnm_terminal_read_json_field(terminal_timeline_kind
        "${terminal_timeline_sample_text}" "${terminal_timeline}" kind)
    vnm_terminal_read_json_field(terminal_timeline_interval_ms
        "${terminal_timeline_sample_text}" "${terminal_timeline}" interval_ms)
    vnm_terminal_read_json_field(terminal_timeline_elapsed_ms
        "${terminal_timeline_sample_text}" "${terminal_timeline}" elapsed_ms)
    vnm_terminal_read_json_field(terminal_timeline_runtime_elapsed_ms
        "${terminal_timeline_sample_text}" "${terminal_timeline}" runtime_metrics elapsed_ms)
    vnm_terminal_read_json_field(terminal_timeline_runtime_schema
        "${terminal_timeline_sample_text}" "${terminal_timeline}" runtime_metrics schema)
    vnm_terminal_read_json_field(terminal_timeline_app_result_available
        "${terminal_timeline_sample_text}" "${terminal_timeline}" app_result_available)

    if(NOT terminal_timeline_schema STREQUAL "vnm_terminal_metrics_timeline_sample_v1")
        message(FATAL_ERROR
            "terminal metrics timeline reports unexpected schema: ${terminal_timeline_schema}")
    endif()

    if(NOT terminal_timeline_runtime_schema STREQUAL "vnm_terminal_runtime_metrics_v2")
        message(FATAL_ERROR
            "terminal metrics timeline reports unexpected runtime_metrics schema: "
            "${terminal_timeline_runtime_schema}")
    endif()
    vnm_terminal_expect_presentation_counter_metadata(
        "${terminal_timeline_sample_text}"
        "${terminal_timeline}"
        runtime_metrics presentation)

    if(NOT terminal_timeline_sample_index STREQUAL "${terminal_timeline_index}")
        message(FATAL_ERROR
            "terminal metrics timeline sample_index=${terminal_timeline_sample_index}, "
            "expected ${terminal_timeline_index}")
    endif()

    if(NOT terminal_timeline_interval_ms STREQUAL "${metrics_interval_ms}")
        message(FATAL_ERROR
            "terminal metrics timeline interval_ms=${terminal_timeline_interval_ms}, "
            "expected ${metrics_interval_ms}")
    endif()

    if(NOT terminal_timeline_runtime_elapsed_ms STREQUAL "${terminal_timeline_elapsed_ms}")
        message(FATAL_ERROR
            "terminal metrics timeline sample elapsed_ms=${terminal_timeline_elapsed_ms}, "
            "runtime_metrics.elapsed_ms=${terminal_timeline_runtime_elapsed_ms}")
    endif()

    if(terminal_timeline_index LESS terminal_timeline_last_index)
        if(NOT terminal_timeline_kind STREQUAL "periodic")
            message(FATAL_ERROR
                "terminal metrics timeline sample ${terminal_timeline_index} has "
                "kind='${terminal_timeline_kind}', expected periodic")
        endif()

        if(terminal_timeline_app_result_available)
            message(FATAL_ERROR
                "terminal metrics timeline periodic sample ${terminal_timeline_index} "
                "reports app_result_available=true")
        endif()

        vnm_terminal_expect_json_missing(
            "${terminal_timeline_sample_text}"
            "${terminal_timeline}"
            runtime_metrics app_result)
        math(EXPR terminal_timeline_periodic_count
            "${terminal_timeline_periodic_count} + 1")
    else()
        if(NOT terminal_timeline_kind STREQUAL "final")
            message(FATAL_ERROR
                "terminal metrics timeline final sample has kind='${terminal_timeline_kind}'")
        endif()

        if(NOT terminal_timeline_app_result_available)
            message(FATAL_ERROR
                "terminal metrics timeline final sample reports app_result_available=false")
        endif()

        vnm_terminal_expect_json_number(
            "${terminal_timeline_sample_text}"
            "${terminal_timeline}"
            runtime_metrics app_result)
    endif()
endforeach()

if(terminal_timeline_periodic_count LESS benchmark_min_windows)
    message(FATAL_ERROR
        "terminal metrics timeline produced ${terminal_timeline_periodic_count} "
        "periodic samples, expected at least ${benchmark_min_windows} to compare "
        "the same number of benchmark windows. Increase "
        "VNM_TERMINAL_CMDG_NELOSTIE_FRAMES or reduce "
        "VNM_TERMINAL_CMDG_BENCHMARK_WINDOW_MS.")
endif()

list(GET terminal_timeline_lines ${terminal_timeline_last_index} terminal_timeline_final_text)

vnm_terminal_read_json_field(terminal_timeline_sample_index
    "${terminal_timeline_final_text}" "${terminal_timeline}" sample_index)
vnm_terminal_read_json_field(terminal_elapsed_ms
    "${terminal_metrics_text}" "${terminal_metrics}" elapsed_ms)
vnm_terminal_read_json_field(terminal_timeline_elapsed_ms
    "${terminal_timeline_final_text}" "${terminal_timeline}" runtime_metrics elapsed_ms)
vnm_terminal_read_json_field(terminal_evidence_frame_count
    "${terminal_metrics_text}" "${terminal_metrics}" renderer_frame_evidence frame_count)
vnm_terminal_read_json_field(terminal_timeline_evidence_frame_count
    "${terminal_timeline_final_text}" "${terminal_timeline}"
    runtime_metrics renderer_frame_evidence frame_count)
vnm_terminal_read_json_field(terminal_presentation_frame_count
    "${terminal_metrics_text}" "${terminal_metrics}" presentation primary_frame_count)
vnm_terminal_read_json_field(terminal_timeline_presentation_frame_count
    "${terminal_timeline_final_text}" "${terminal_timeline}"
    runtime_metrics presentation primary_frame_count)

if(NOT terminal_timeline_sample_index STREQUAL "${terminal_timeline_last_index}")
    message(FATAL_ERROR
        "terminal metrics timeline final sample_index=${terminal_timeline_sample_index}, "
        "expected ${terminal_timeline_last_index}")
endif()

if(NOT terminal_timeline_elapsed_ms STREQUAL "${terminal_elapsed_ms}")
    message(FATAL_ERROR
        "terminal metrics timeline final runtime elapsed_ms=${terminal_timeline_elapsed_ms}, "
        "expected aggregate elapsed_ms=${terminal_elapsed_ms}")
endif()

if(NOT terminal_timeline_evidence_frame_count STREQUAL "${terminal_evidence_frame_count}")
    message(FATAL_ERROR
        "terminal metrics timeline renderer evidence frame_count="
        "${terminal_timeline_evidence_frame_count}, expected ${terminal_evidence_frame_count}")
endif()

if(NOT terminal_timeline_presentation_frame_count STREQUAL "${terminal_presentation_frame_count}")
    message(FATAL_ERROR
        "terminal metrics timeline presentation primary_frame_count="
        "${terminal_timeline_presentation_frame_count}, expected "
        "${terminal_presentation_frame_count}")
endif()

vnm_terminal_read_json_field(cmdg_scene
    "${cmdg_metrics_text}" "${cmdg_metrics}" scene)
vnm_terminal_read_json_field(cmdg_exit_reason
    "${cmdg_metrics_text}" "${cmdg_metrics}" exit_reason)
vnm_terminal_read_json_field(cmdg_hide_cursor
    "${cmdg_metrics_text}" "${cmdg_metrics}" hide_cursor)
vnm_terminal_read_json_field(cmdg_scene_frames
    "${cmdg_metrics_text}" "${cmdg_metrics}" scene_frames)

if(NOT cmdg_scene STREQUAL "${scene}")
    message(FATAL_ERROR
        "CMDG metrics JSON reports scene '${cmdg_scene}', expected '${scene}'")
endif()

if(NOT cmdg_exit_reason STREQUAL "frame_limit")
    message(FATAL_ERROR
        "CMDG metrics JSON reports exit_reason='${cmdg_exit_reason}', expected 'frame_limit'")
endif()

if(hide_cursor)
    if(NOT cmdg_hide_cursor)
        message(FATAL_ERROR
            "CMDG metrics JSON reports hide_cursor='${cmdg_hide_cursor}', expected true")
    endif()
elseif(cmdg_hide_cursor)
    message(FATAL_ERROR
        "CMDG metrics JSON reports hide_cursor='${cmdg_hide_cursor}', expected false")
endif()

if(NOT cmdg_scene_frames STREQUAL "${frame_limit}")
    message(FATAL_ERROR
        "CMDG metrics JSON reports scene_frames=${cmdg_scene_frames}, "
        "expected ${frame_limit}")
endif()

string(JSON cmdg_benchmark_windows_type
    ERROR_VARIABLE cmdg_benchmark_windows_error
    TYPE "${cmdg_metrics_text}" benchmark_windows)
if(cmdg_benchmark_windows_error STREQUAL "NOTFOUND")
    if(NOT cmdg_benchmark_windows_type STREQUAL "OBJECT")
        message(FATAL_ERROR
            "CMDG metrics JSON reports benchmark_windows type "
            "${cmdg_benchmark_windows_type}, expected OBJECT")
    endif()
    set(cmdg_benchmark_windows_available ON)
else()
    set(cmdg_benchmark_windows_available OFF)
    message(STATUS
        "CMDG metrics JSON has no benchmark_windows object; accepting older "
        "scratch CMDG metrics without window samples.")
endif()

if(cmdg_benchmark_windows_available)
vnm_terminal_read_json_field(cmdg_window_schema
    "${cmdg_metrics_text}" "${cmdg_metrics}" benchmark_windows schema)
vnm_terminal_read_json_field(cmdg_window_interval_ms
    "${cmdg_metrics_text}" "${cmdg_metrics}" benchmark_windows interval_ms)
vnm_terminal_read_json_length(cmdg_window_count
    "${cmdg_metrics_text}" "${cmdg_metrics}" benchmark_windows samples)

if(NOT cmdg_window_schema STREQUAL "cmdg_benchmark_windows_v1")
    message(FATAL_ERROR
        "CMDG metrics JSON reports unexpected benchmark_windows.schema: "
        "${cmdg_window_schema}")
endif()

if(NOT cmdg_window_interval_ms STREQUAL "${metrics_interval_ms}")
    message(FATAL_ERROR
        "CMDG metrics JSON reports benchmark_windows.interval_ms="
        "${cmdg_window_interval_ms}, expected ${metrics_interval_ms}")
endif()

if(NOT cmdg_window_count GREATER 0)
    message(FATAL_ERROR "CMDG metrics JSON reports no benchmark windows")
endif()

if(cmdg_window_count LESS benchmark_min_windows)
    message(FATAL_ERROR
        "CMDG metrics JSON reports ${cmdg_window_count} benchmark windows, expected "
        "at least ${benchmark_min_windows}. Increase "
        "VNM_TERMINAL_CMDG_NELOSTIE_FRAMES or reduce "
        "VNM_TERMINAL_CMDG_BENCHMARK_WINDOW_MS.")
endif()
endif()

vnm_terminal_read_json_field(cmdg_draw_frames
    "${cmdg_metrics_text}" "${cmdg_metrics}" draw_frames)
vnm_terminal_read_json_field(cmdg_scene_calc_ms_total
    "${cmdg_metrics_text}" "${cmdg_metrics}" scene_calc_ms_total)
vnm_terminal_read_json_field(cmdg_scene_wait_ms_total
    "${cmdg_metrics_text}" "${cmdg_metrics}" scene_wait_ms_total)
vnm_terminal_read_json_field(cmdg_draw_ms_total
    "${cmdg_metrics_text}" "${cmdg_metrics}" draw_ms_total)
vnm_terminal_read_json_field(cmdg_draw_wait_ms_total
    "${cmdg_metrics_text}" "${cmdg_metrics}" draw_wait_ms_total)
vnm_terminal_read_json_field(cmdg_draw_output_bytes
    "${cmdg_metrics_text}" "${cmdg_metrics}" draw_output_bytes)
vnm_terminal_read_json_field(cmdg_changed_rows
    "${cmdg_metrics_text}" "${cmdg_metrics}" changed_rows)
vnm_terminal_read_json_field(cmdg_changed_cells
    "${cmdg_metrics_text}" "${cmdg_metrics}" changed_cells)

foreach(cmdg_number_field IN ITEMS
    elapsed_ms
    scene_elapsed_seconds
    scene_frames
    draw_frames
    scene_frames_per_second
    draw_frames_per_second
    scene_calc_ms_total
    scene_wait_ms_total
    draw_ms_total
    draw_pre_write_build_ms_total
    draw_open_stdout_ms_total
    draw_utf8_encode_ms_total
    draw_stdout_write_flush_ms_total
    draw_wait_ms_total
    draw_output_bytes
    changed_rows
    changed_cells)
    vnm_terminal_expect_json_number(
        "${cmdg_metrics_text}"
        "${cmdg_metrics}"
        ${cmdg_number_field})
endforeach()

if(cmdg_benchmark_windows_available)
set(cmdg_window_scene_frames 0)
set(cmdg_window_draw_frames 0)
set(cmdg_window_scene_calc_ms_total 0)
set(cmdg_window_scene_wait_ms_total 0)
set(cmdg_window_draw_ms_total 0)
set(cmdg_window_draw_wait_ms_total 0)
set(cmdg_window_draw_output_bytes 0)
set(cmdg_window_changed_rows 0)
set(cmdg_window_changed_cells 0)
math(EXPR cmdg_window_last_index "${cmdg_window_count} - 1")
foreach(cmdg_window_index RANGE 0 ${cmdg_window_last_index})
    vnm_terminal_read_json_field(window_index
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} index)
    if(NOT window_index STREQUAL "${cmdg_window_index}")
        message(FATAL_ERROR
            "CMDG metrics JSON reports benchmark window index=${window_index}, "
            "expected ${cmdg_window_index}")
    endif()

    foreach(window_number_field IN ITEMS
        start_elapsed_ms
        end_elapsed_ms
        scene_frames
        draw_frames
        scene_elapsed_seconds_start
        scene_elapsed_seconds_end
        scene_calc_ms_total
        scene_wait_ms_total
        draw_ms_total
        draw_pre_write_build_ms_total
        draw_open_stdout_ms_total
        draw_utf8_encode_ms_total
        draw_stdout_write_flush_ms_total
        draw_wait_ms_total
        draw_output_bytes
        changed_rows
        changed_cells
        scene_frames_per_second
        draw_frames_per_second)
        vnm_terminal_expect_json_number(
            "${cmdg_metrics_text}"
            "${cmdg_metrics}"
            benchmark_windows samples ${cmdg_window_index} ${window_number_field})
    endforeach()

    vnm_terminal_read_json_field(window_start_elapsed_ms
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} start_elapsed_ms)
    vnm_terminal_read_json_field(window_end_elapsed_ms
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} end_elapsed_ms)
    if(window_end_elapsed_ms LESS window_start_elapsed_ms)
        message(FATAL_ERROR
            "CMDG metrics JSON benchmark window ${cmdg_window_index} has "
            "end_elapsed_ms=${window_end_elapsed_ms} before "
            "start_elapsed_ms=${window_start_elapsed_ms}")
    endif()

    vnm_terminal_read_json_field(window_scene_frames
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} scene_frames)
    vnm_terminal_read_json_field(window_draw_frames
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} draw_frames)
    vnm_terminal_read_json_field(window_scene_calc_ms_total
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} scene_calc_ms_total)
    vnm_terminal_read_json_field(window_scene_wait_ms_total
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} scene_wait_ms_total)
    vnm_terminal_read_json_field(window_draw_ms_total
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} draw_ms_total)
    vnm_terminal_read_json_field(window_draw_wait_ms_total
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} draw_wait_ms_total)
    vnm_terminal_read_json_field(window_draw_output_bytes
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} draw_output_bytes)
    vnm_terminal_read_json_field(window_changed_rows
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} changed_rows)
    vnm_terminal_read_json_field(window_changed_cells
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} changed_cells)
    vnm_terminal_read_json_field(window_scene_frames_per_second
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} scene_frames_per_second)
    vnm_terminal_read_json_field(window_draw_frames_per_second
        "${cmdg_metrics_text}" "${cmdg_metrics}"
        benchmark_windows samples ${cmdg_window_index} draw_frames_per_second)

    if(window_scene_frames GREATER 0 AND
        NOT window_scene_frames_per_second GREATER 0)
        message(FATAL_ERROR
            "CMDG metrics JSON benchmark window ${cmdg_window_index} has "
            "scene_frames=${window_scene_frames} but non-positive scene FPS")
    endif()

    if(window_draw_frames GREATER 0 AND
        NOT window_draw_frames_per_second GREATER 0)
        message(FATAL_ERROR
            "CMDG metrics JSON benchmark window ${cmdg_window_index} has "
            "draw_frames=${window_draw_frames} but non-positive draw FPS")
    endif()

    math(EXPR cmdg_window_scene_frames
        "${cmdg_window_scene_frames} + ${window_scene_frames}")
    math(EXPR cmdg_window_draw_frames
        "${cmdg_window_draw_frames} + ${window_draw_frames}")
    math(EXPR cmdg_window_scene_calc_ms_total
        "${cmdg_window_scene_calc_ms_total} + ${window_scene_calc_ms_total}")
    math(EXPR cmdg_window_scene_wait_ms_total
        "${cmdg_window_scene_wait_ms_total} + ${window_scene_wait_ms_total}")
    math(EXPR cmdg_window_draw_wait_ms_total
        "${cmdg_window_draw_wait_ms_total} + ${window_draw_wait_ms_total}")
    math(EXPR cmdg_window_draw_output_bytes
        "${cmdg_window_draw_output_bytes} + ${window_draw_output_bytes}")
    math(EXPR cmdg_window_changed_rows
        "${cmdg_window_changed_rows} + ${window_changed_rows}")
    math(EXPR cmdg_window_changed_cells
        "${cmdg_window_changed_cells} + ${window_changed_cells}")
endforeach()

foreach(cmdg_counter_name IN ITEMS
    scene_frames
    draw_frames
    scene_calc_ms_total
    scene_wait_ms_total
    draw_wait_ms_total
    draw_output_bytes
    changed_rows
    changed_cells)
    if(NOT cmdg_window_${cmdg_counter_name} STREQUAL "${cmdg_${cmdg_counter_name}}")
        message(FATAL_ERROR
            "CMDG benchmark window ${cmdg_counter_name} sum="
            "${cmdg_window_${cmdg_counter_name}}, expected ${cmdg_${cmdg_counter_name}}")
    endif()
endforeach()
endif()

message(STATUS "CMDG scene: ${scene} (${run_id})")
message(STATUS "terminal metrics: ${terminal_metrics}")
message(STATUS "terminal timeline: ${terminal_timeline}")
message(STATUS "CMDG metrics: ${cmdg_metrics}")
if(profile_text)
    message(STATUS "terminal profile: ${profile_text}")
endif()
