set(VNM_TERMINAL_CMDG_NELOSTIE_EXE "" CACHE FILEPATH
    "Optional path to prebuilt CMDG.exe for the AssemblyWinter2025/Nelostie benchmark.")
set(VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR "" CACHE PATH
    "Working directory for CMDG.exe. Defaults to the executable directory.")
set(VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR
    "${PROJECT_SOURCE_DIR}/third_party/CMDG"
    CACHE PATH
    "Source checkout for the vendored CMDG AssemblyWinter2025/Nelostie benchmark workload.")
set(VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS "180" CACHE STRING
    "Timeout for the CMDG Nelostie benchmark.")
set(VNM_TERMINAL_CMDG_NELOSTIE_FRAMES "300" CACHE STRING
    "Number of CMDG scene frames to run before exiting.")
set(VNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE "1920x1080" CACHE STRING
    "vnm_terminal window size for the CMDG Nelostie benchmark.")
option(VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER
    "Run the CMDG Nelostie benchmark with the Qt software renderer."
    OFF)
option(VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN
    "Run the CMDG Nelostie benchmark with the Qt offscreen platform."
    OFF)

if(VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN)
    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)
    set(vnm_terminal_cmdg_offscreen_plugin_dir
        "$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
else()
    set(vnm_terminal_cmdg_offscreen_plugin_dir "")
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
    if(VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR STREQUAL "")
        get_filename_component(
            vnm_terminal_cmdg_nelostie_working_dir
            "${vnm_terminal_cmdg_nelostie_exe}"
            DIRECTORY)
    else()
        set(vnm_terminal_cmdg_nelostie_working_dir
            "${VNM_TERMINAL_CMDG_NELOSTIE_WORKING_DIR}")
    endif()

    add_test(
        NAME vnm_terminal_cmdg_nelostie_benchmark
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dterminal_exe=$<TARGET_FILE:vnm_terminal>"
            "-Dcmdg_exe=${vnm_terminal_cmdg_nelostie_exe}"
            "-Dcmdg_working_dir=${vnm_terminal_cmdg_nelostie_working_dir}"
            "-Dcmdg_source_dir=${VNM_TERMINAL_CMDG_NELOSTIE_SOURCE_DIR}"
            "-Dbuild_cmdg=$<BOOL:${vnm_terminal_cmdg_nelostie_build_from_source}>"
            "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}"
            "-Dtimeout_seconds=${VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS}"
            "-Dframe_limit=${VNM_TERMINAL_CMDG_NELOSTIE_FRAMES}"
            "-Dwindow_size=${VNM_TERMINAL_CMDG_NELOSTIE_WINDOW_SIZE}"
            "-Dqt_bin_dir=$<TARGET_FILE_DIR:Qt6::Core>"
            "-Doffscreen_plugin_dir=${vnm_terminal_cmdg_offscreen_plugin_dir}"
            "-Dsoftware_renderer=$<BOOL:${VNM_TERMINAL_CMDG_NELOSTIE_SOFTWARE_RENDERER}>"
            "-Doffscreen=$<BOOL:${VNM_TERMINAL_CMDG_NELOSTIE_OFFSCREEN}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_cmdg_nelostie.cmake"
    )

    math(EXPR vnm_terminal_cmdg_nelostie_ctest_timeout
        "${VNM_TERMINAL_CMDG_NELOSTIE_TIMEOUT_SECONDS} + 30")

    set_tests_properties(vnm_terminal_cmdg_nelostie_benchmark PROPERTIES
        LABELS "benchmark;cmdg;nelostie"
        TIMEOUT "${vnm_terminal_cmdg_nelostie_ctest_timeout}"
    )
endif()
