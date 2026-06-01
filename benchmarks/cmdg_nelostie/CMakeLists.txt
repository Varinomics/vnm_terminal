set(VNM_TERMINAL_CMDG_NELOSTIE_EXE "" CACHE FILEPATH
    "Optional path to prebuilt CMDG.exe for the AssemblyWinter2025/Nelostie benchmark.")
set(VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR "" CACHE PATH
    "Working directory for CMDG.exe. Defaults to the executable directory.")
set(VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/THIRD_PARTY/CMDG"
    CACHE PATH
    "Source checkout for the vendored CMDG AssemblyWinter2025/Nelostie benchmark workload.")
set(VNM_TERMINAL_CMDG_NELOSTIE_PROFILE_TEXT "" CACHE FILEPATH
    "Optional profile text output path for the CMDG Nelostie benchmark.")
if(VNM_TERMINAL_CMDG_NELOSTIE_PROFILE_TEXT)
    set(vnm_terminal_cmdg_default_scenes "AssemblyWinter2025")
else()
    set(vnm_terminal_cmdg_default_scenes
        "AssemblyWinter2025;Example2D;Plasma;JuliaSetTest;ParticleVortex;Example3D")
endif()
set(VNM_TERMINAL_CMDG_SCENES
    "${vnm_terminal_cmdg_default_scenes}"
    CACHE STRING
    "Semicolon-separated CMDG scenes for the benchmark suite.")
set(VNM_TERMINAL_CMDG_REPEAT_COUNT "1" CACHE STRING
    "Number of times to run each configured CMDG benchmark scene.")
set(VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS "180" CACHE STRING
    "Timeout for the CMDG Nelostie benchmark.")
set(VNM_TERMINAL_CMDG_NELOSTIE_FRAMES "300" CACHE STRING
    "Number of CMDG scene frames to run before exiting.")
set(VNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE "1920x1080" CACHE STRING
    "vnm_terminal window size for the CMDG Nelostie benchmark.")
set(VNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE "10" CACHE STRING
    "vnm_terminal font pixel size for CMDG benchmark runs.")
option(VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER
    "Run the CMDG Nelostie benchmark with the Qt software renderer."
    OFF)
option(VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN
    "Run the CMDG Nelostie benchmark with the Qt offscreen platform."
    OFF)
option(VNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR
    "Ask CMDG to hide the terminal cursor during benchmark scene frames."
    ON)

if(VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN)
    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)
    set(vnm_terminal_cmdg_offscreen_plugin_dir
        "$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
else()
    set(vnm_terminal_cmdg_offscreen_plugin_dir "")
endif()

set(vnm_terminal_cmdg_supported_scenes
    AssemblyWinter2025
    ContentWiggler
    Example2D
    Example2D_characters
    Example3D
    JuliaSetTest
    NoiseTest
    ParticleVortex
    Plasma
    QuickHello
    RotateTutorial
    SceneTemplate
)

set(vnm_terminal_cmdg_scenes "${VNM_TERMINAL_CMDG_SCENES}")
string(REPLACE "," ";" vnm_terminal_cmdg_scenes "${vnm_terminal_cmdg_scenes}")
list(TRANSFORM vnm_terminal_cmdg_scenes STRIP)
list(REMOVE_ITEM vnm_terminal_cmdg_scenes "")

if(NOT vnm_terminal_cmdg_scenes)
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_SCENES must name at least one CMDG scene")
endif()

if(NOT VNM_TERMINAL_CMDG_REPEAT_COUNT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_REPEAT_COUNT must be a positive integer")
endif()

if(NOT VNM_TERMINAL_CMDG_NELOSTIE_FRAMES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_NELOSTIE_FRAMES must be a positive integer")
endif()

if(NOT VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS must be a positive integer")
endif()

if(NOT VNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE MATCHES "^[0-9]+(\\.[0-9]+)?$" OR
    VNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE MATCHES "^0+(\\.0+)?$")
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE must be a positive pixel size")
endif()

set(vnm_terminal_cmdg_seen_scenes)
foreach(vnm_terminal_cmdg_scene IN LISTS vnm_terminal_cmdg_scenes)
    if(NOT vnm_terminal_cmdg_scene MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "CMDG scene names must be simple class names: ${vnm_terminal_cmdg_scene}")
    endif()

    list(FIND vnm_terminal_cmdg_supported_scenes "${vnm_terminal_cmdg_scene}"
        vnm_terminal_cmdg_scene_index)
    if(vnm_terminal_cmdg_scene_index EQUAL -1)
        list(JOIN vnm_terminal_cmdg_supported_scenes ", " vnm_terminal_cmdg_supported_text)
        message(FATAL_ERROR
            "Unsupported CMDG scene '${vnm_terminal_cmdg_scene}'. "
            "Supported scenes: ${vnm_terminal_cmdg_supported_text}")
    endif()

    list(FIND vnm_terminal_cmdg_seen_scenes "${vnm_terminal_cmdg_scene}"
        vnm_terminal_cmdg_duplicate_index)
    if(NOT vnm_terminal_cmdg_duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Duplicate CMDG scene '${vnm_terminal_cmdg_scene}' in "
            "VNM_TERMINAL_CMDG_SCENES; use VNM_TERMINAL_CMDG_REPEAT_COUNT for repeats")
    endif()
    list(APPEND vnm_terminal_cmdg_seen_scenes "${vnm_terminal_cmdg_scene}")
endforeach()

list(LENGTH vnm_terminal_cmdg_scenes vnm_terminal_cmdg_scene_count)
math(EXPR vnm_terminal_cmdg_run_count
    "${vnm_terminal_cmdg_scene_count} * ${VNM_TERMINAL_CMDG_REPEAT_COUNT}")

if(VNM_TERMINAL_CMDG_NELOSTIE_PROFILE_TEXT AND
    NOT vnm_terminal_cmdg_run_count EQUAL 1)
    message(FATAL_ERROR
        "VNM_TERMINAL_CMDG_NELOSTIE_PROFILE_TEXT is an exact file path and "
        "cannot be used with multiple CMDG scenes or repeats")
endif()

set(vnm_terminal_cmdg_nelostie_exe "${VNM_TERMINAL_CMDG_NELOSTIE_EXE}")
set(vnm_terminal_cmdg_nelostie_build_from_source OFF)

if(vnm_terminal_cmdg_nelostie_exe STREQUAL "" AND
    IS_DIRECTORY "${VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR}")
    set(vnm_terminal_cmdg_nelostie_build_from_source ON)
    set(vnm_terminal_cmdg_nelostie_exe
        "${VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR}/CMDG/bin/Release/net8.0/CMDG.exe")
endif()

if(vnm_terminal_cmdg_nelostie_exe)
    set(vnm_terminal_cmdg_nelostie_profile_text
        "${VNM_TERMINAL_CMDG_NELOSTIE_PROFILE_TEXT}")

    if(VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR STREQUAL "")
        get_filename_component(
            vnm_terminal_cmdg_nelostie_working_dir
            "${vnm_terminal_cmdg_nelostie_exe}"
            DIRECTORY)
    else()
        set(vnm_terminal_cmdg_nelostie_working_dir
            "${VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR}")
    endif()

    math(EXPR vnm_terminal_cmdg_nelostie_ctest_timeout
        "${VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS} + 30")

    foreach(vnm_terminal_cmdg_scene IN LISTS vnm_terminal_cmdg_scenes)
        foreach(vnm_terminal_cmdg_repeat_index RANGE 1 ${VNM_TERMINAL_CMDG_REPEAT_COUNT})
            set(vnm_terminal_cmdg_run_id
                "${vnm_terminal_cmdg_scene}_r${vnm_terminal_cmdg_repeat_index}")
            set(vnm_terminal_cmdg_output_dir
                "${CMAKE_CURRENT_BINARY_DIR}/${vnm_terminal_cmdg_scene}/repeat_${vnm_terminal_cmdg_repeat_index}")

            if(vnm_terminal_cmdg_scene STREQUAL "AssemblyWinter2025" AND
                vnm_terminal_cmdg_repeat_index EQUAL 1)
                set(vnm_terminal_cmdg_test_name vnm_terminal_cmdg_nelostie_benchmark)
            else()
                set(vnm_terminal_cmdg_test_name
                    "vnm_terminal_cmdg_${vnm_terminal_cmdg_run_id}_benchmark")
            endif()

            if(vnm_terminal_cmdg_nelostie_profile_text)
                set(vnm_terminal_cmdg_profile_text
                    "${vnm_terminal_cmdg_nelostie_profile_text}")
            elseif(VNM_TERMINAL_ENABLE_PROFILING)
                set(vnm_terminal_cmdg_profile_text
                    "${vnm_terminal_cmdg_output_dir}/vnm_terminal_cmdg_${vnm_terminal_cmdg_run_id}_profile.txt")
            else()
                set(vnm_terminal_cmdg_profile_text "")
            endif()

            add_test(
                NAME ${vnm_terminal_cmdg_test_name}
                COMMAND
                    "${CMAKE_COMMAND}"
                    "-Dterminal_exe=$<TARGET_FILE:vnm_terminal>"
                    "-Dcmdg_exe=${vnm_terminal_cmdg_nelostie_exe}"
                    "-Dcmdg_working_dir=${vnm_terminal_cmdg_nelostie_working_dir}"
                    "-Dcmdg_source_dir=${VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR}"
                    "-Dbuild_cmdg=$<BOOL:${vnm_terminal_cmdg_nelostie_build_from_source}>"
                    "-Dbuild_lock_path=${CMAKE_CURRENT_BINARY_DIR}/cmdg_build.lock"
                    "-Dscene=${vnm_terminal_cmdg_scene}"
                    "-Drun_id=${vnm_terminal_cmdg_run_id}"
                    "-Doutput_dir=${vnm_terminal_cmdg_output_dir}"
                    "-Dprofile_text=${vnm_terminal_cmdg_profile_text}"
                    "-Dtimeout_seconds=${VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS}"
                    "-Dframe_limit=${VNM_TERMINAL_CMDG_NELOSTIE_FRAMES}"
                    "-Dwindow_size=${VNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE}"
                    "-Dfont_size=${VNM_TERMINAL_CMDG_NELOSTIE_FONT_SIZE}"
                    "-Dhide_cursor=$<BOOL:${VNM_TERMINAL_CMDG_NELOSTIE_HIDE_CURSOR}>"
                    "-Dqt_bin_dir=$<TARGET_FILE_DIR:Qt6::Core>"
                    "-Doffscreen_plugin_dir=${vnm_terminal_cmdg_offscreen_plugin_dir}"
                    "-Dsoftware_renderer=$<BOOL:${VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER}>"
                    "-Doffscreen=$<BOOL:${VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cmdg_nelostie.cmake"
            )

            set(vnm_terminal_cmdg_test_labels benchmark cmdg cmdg_suite)
            if(vnm_terminal_cmdg_scene STREQUAL "AssemblyWinter2025")
                list(APPEND vnm_terminal_cmdg_test_labels nelostie)
            endif()
            list(APPEND vnm_terminal_cmdg_test_labels "${vnm_terminal_cmdg_scene}")

            set_tests_properties(${vnm_terminal_cmdg_test_name} PROPERTIES
                LABELS "${vnm_terminal_cmdg_test_labels}"
                RUN_SERIAL TRUE
                TIMEOUT "${vnm_terminal_cmdg_nelostie_ctest_timeout}"
            )
        endforeach()
    endforeach()
endif()
