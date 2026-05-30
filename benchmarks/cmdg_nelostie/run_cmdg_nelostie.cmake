if(NOT EXISTS "${terminal_exe}")
    message(FATAL_ERROR "vnm_terminal executable does not exist: ${terminal_exe}")
endif()

if(build_cmdg)
    if(NOT IS_DIRECTORY "${cmdg_source_dir}")
        message(FATAL_ERROR "CMDG source directory does not exist: ${cmdg_source_dir}")
    endif()

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

if(NOT IS_DIRECTORY "${cmdg_working_dir}")
    message(FATAL_ERROR "CMDG working directory does not exist: ${cmdg_working_dir}")
endif()

file(MAKE_DIRECTORY "${output_dir}")
set(terminal_metrics "${output_dir}/vnm_terminal_cmdg_nelostie_terminal_metrics.json")
set(cmdg_metrics "${output_dir}/vnm_terminal_cmdg_nelostie_cmdg_metrics.json")
file(REMOVE "${terminal_metrics}" "${cmdg_metrics}")

math(EXPR timeout_ms "${timeout_seconds} * 1000")

set(terminal_arguments
    "${terminal_exe}"
    "--metrics-json" "${terminal_metrics}"
    "--window-size" "${window_size}"
    "--timeout-ms" "${timeout_ms}"
    "--require-output"
    "--cwd" "${cmdg_working_dir}")

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
set(ENV{CMDG_SCENE} "AssemblyWinter2025")
set(ENV{CMDG_DISABLE_AUDIO} "1")
set(ENV{CMDG_ADJUST_SCREEN} "0")
set(ENV{CMDG_SPLASH_SCREEN} "0")
set(ENV{CMDG_BENCHMARK_FRAME_LIMIT} "${frame_limit}")
set(ENV{CMDG_BENCHMARK_METRICS} "${cmdg_metrics}")

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
    message(FATAL_ERROR "CMDG Nelostie benchmark failed with exit code ${result}")
endif()

if(NOT EXISTS "${terminal_metrics}")
    message(FATAL_ERROR "terminal metrics JSON was not written: ${terminal_metrics}")
endif()

if(NOT EXISTS "${cmdg_metrics}")
    message(FATAL_ERROR "CMDG metrics JSON was not written: ${cmdg_metrics}")
endif()

file(READ "${terminal_metrics}" terminal_metrics_text)
file(READ "${cmdg_metrics}" cmdg_metrics_text)

if(NOT terminal_metrics_text MATCHES "\"paint_completed_frames\"")
    message(FATAL_ERROR "terminal metrics JSON does not contain paint_completed_frames")
endif()

if(NOT cmdg_metrics_text MATCHES "\"scene_frames\"")
    message(FATAL_ERROR "CMDG metrics JSON does not contain scene_frames")
endif()

if(NOT terminal_metrics_text MATCHES "\"backend_error_count\"[ \r\n]*:[ \r\n]*0")
    message(FATAL_ERROR "terminal metrics JSON reports backend errors")
endif()

if(NOT terminal_metrics_text MATCHES "\"timeout_expired\"[ \r\n]*:[ \r\n]*false")
    message(FATAL_ERROR "terminal metrics JSON reports a timeout")
endif()

if(NOT terminal_metrics_text MATCHES "\"paint_completed_frames\"[ \r\n]*:[ \r\n]*\"?[1-9][0-9]*\"?")
    message(FATAL_ERROR "terminal metrics JSON reports no painted frames")
endif()

if(NOT cmdg_metrics_text MATCHES "\"exit_reason\"[ \r\n]*:[ \r\n]*\"frame_limit\"")
    message(FATAL_ERROR "CMDG metrics JSON did not exit via frame_limit")
endif()

if(NOT cmdg_metrics_text MATCHES "\"scene_frames\"[ \r\n]*:[ \r\n]*${frame_limit}")
    message(FATAL_ERROR
        "CMDG metrics JSON does not report the expected frame limit ${frame_limit}")
endif()

message(STATUS "terminal metrics: ${terminal_metrics}")
message(STATUS "CMDG metrics: ${cmdg_metrics}")
