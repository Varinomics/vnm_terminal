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
    set(cmdg_metrics "${output_dir}/vnm_terminal_cmdg_nelostie_cmdg_metrics.json")
else()
    set(terminal_metrics "${output_dir}/vnm_terminal_cmdg_${run_id}_terminal_metrics.json")
    set(cmdg_metrics "${output_dir}/vnm_terminal_cmdg_${run_id}_cmdg_metrics.json")
endif()
file(REMOVE "${terminal_metrics}" "${cmdg_metrics}")
if(profile_text)
    file(REMOVE "${profile_text}")
endif()

math(EXPR timeout_ms "${timeout_seconds} * 1000")

set(terminal_arguments
    "${terminal_exe}"
    "--metrics-json" "${terminal_metrics}"
    "--font-size" "${font_size}"
    "--window-size" "${window_size}"
    "--timeout-ms" "${timeout_ms}"
    "--require-output"
    "--cwd" "${cmdg_working_dir}")

if(profile_text)
    list(APPEND terminal_arguments "--profile-text" "${profile_text}")
endif()

if(software_renderer)
    list(APPEND terminal_arguments "--software-renderer")
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

if(NOT EXISTS "${cmdg_metrics}")
    message(FATAL_ERROR "CMDG metrics JSON was not written: ${cmdg_metrics}")
endif()

if(profile_text AND NOT EXISTS "${profile_text}")
    message(FATAL_ERROR "terminal profile text was not written: ${profile_text}")
endif()

file(READ "${terminal_metrics}" terminal_metrics_text)
file(READ "${cmdg_metrics}" cmdg_metrics_text)

vnm_terminal_read_json_field(terminal_backend_error_count
    "${terminal_metrics_text}" "${terminal_metrics}" backend_error_count)
vnm_terminal_read_json_field(terminal_timeout_expired
    "${terminal_metrics_text}" "${terminal_metrics}" timeout_expired)
vnm_terminal_read_json_field(terminal_paint_completed_frames
    "${terminal_metrics_text}" "${terminal_metrics}" renderer paint_completed_frames)

if(NOT terminal_backend_error_count STREQUAL "0")
    message(FATAL_ERROR
        "terminal metrics JSON reports backend_error_count=${terminal_backend_error_count}")
endif()

if(terminal_timeout_expired)
    message(FATAL_ERROR "terminal metrics JSON reports a timeout")
endif()

if(NOT terminal_paint_completed_frames MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "terminal metrics JSON reports no painted frames")
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

message(STATUS "CMDG scene: ${scene} (${run_id})")
message(STATUS "terminal metrics: ${terminal_metrics}")
message(STATUS "CMDG metrics: ${cmdg_metrics}")
if(profile_text)
    message(STATUS "terminal profile: ${profile_text}")
endif()
