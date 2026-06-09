if(WIN32)
    set(vnm_terminal_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)

function(vnm_terminal_require_qt_plugin_file plugin_target purpose)
    if(NOT TARGET ${plugin_target})
        message(FATAL_ERROR
            "${purpose} requires Qt plugin target ${plugin_target}")
    endif()

    get_target_property(plugin_target_type ${plugin_target} TYPE)
    if(NOT plugin_target_type MATCHES "^(MODULE_LIBRARY|SHARED_LIBRARY)$")
        message(FATAL_ERROR
            "${purpose} requires ${plugin_target} to be a dynamic plugin; target type is ${plugin_target_type}")
    endif()
endfunction()

function(vnm_terminal_qt_offscreen_test_environment out_var)
    set(${out_var}
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        PARENT_SCOPE)
endfunction()

function(vnm_terminal_configure_test_process target_name)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src"
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /FS
        )
    endif()
endfunction()

vnm_terminal_require_qt_plugin_file(
    Qt6::QOffscreenIntegrationPlugin
    "CTest offscreen runs")
vnm_terminal_qt_offscreen_test_environment(vnm_terminal_qt_offscreen_environment)

set(vnm_terminal_qt_scale_environment
    "QT_SCREEN_SCALE_FACTORS="
    "QT_AUTO_SCREEN_SCALE_FACTOR=0"
    "QT_DEVICE_PIXEL_RATIO="
    "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
)

set(vnm_terminal_qt_unscaled_environment
    "QT_SCALE_FACTOR=1"
    ${vnm_terminal_qt_scale_environment}
)

set(vnm_terminal_custom_titlebar_supported OFF)
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL Linux)
    set(vnm_terminal_custom_titlebar_supported ON)
endif()

add_executable(vnm_terminal_title_metadata
    terminal_app/terminal_title_metadata_tests.cpp
)

target_link_libraries(vnm_terminal_title_metadata PRIVATE
    Qt6::Core
)

set_target_properties(vnm_terminal_title_metadata PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_title_metadata
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_title_metadata>"
)

set_tests_properties(vnm_terminal_title_metadata PROPERTIES
    TIMEOUT 30
)

add_executable(vnm_terminal_custom_titlebar_integration
    terminal_app/custom_titlebar_integration_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app_cli.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_clipboard_policy.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_common.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_metrics.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_options.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_profile_text.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_settings.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_shortcuts.cpp"
    "${PROJECT_SOURCE_DIR}/src/qml_chrome.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_scrollbar.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_settings_controller.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_settings_window.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_window.cpp"
)

target_link_libraries(vnm_terminal_custom_titlebar_integration PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    vnm_qml_chrome::vnm_qml_chrome
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickLayouts
    Qt6::QuickControls2
)

target_compile_definitions(vnm_terminal_custom_titlebar_integration PRIVATE
    "VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}>"
)

set_target_properties(vnm_terminal_custom_titlebar_integration PROPERTIES
    AUTOMOC ON
)

add_test(
    NAME vnm_terminal_custom_titlebar_integration
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_custom_titlebar_integration>"
)

set_tests_properties(vnm_terminal_custom_titlebar_integration PROPERTIES
    TIMEOUT 30
)

add_executable(vnm_terminal_app_persistence
    terminal_app/app_persistence_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app_cli.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_clipboard_policy.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_common.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_metrics.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_options.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_profile_text.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_settings.cpp"
    "${PROJECT_SOURCE_DIR}/src/app_shortcuts.cpp"
    "${PROJECT_SOURCE_DIR}/src/qml_chrome.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_scrollbar.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_settings_controller.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_settings_window.cpp"
    "${PROJECT_SOURCE_DIR}/src/terminal_window.cpp"
)

target_link_libraries(vnm_terminal_app_persistence PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    vnm_qml_chrome::vnm_qml_chrome
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickLayouts
    Qt6::QuickControls2
)

set_target_properties(vnm_terminal_app_persistence PROPERTIES
    AUTOMOC ON
)

add_test(
    NAME vnm_terminal_app_persistence
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_app_persistence>"
)

set_tests_properties(vnm_terminal_app_persistence PROPERTIES
    TIMEOUT 30
)

if(TARGET vnm_terminal_canvas_fixture)
    add_test(
        NAME vnm_terminal_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--window-size" "800x480"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_smoke PROPERTIES
        TIMEOUT 30
    )

    set(vnm_terminal_custom_titlebar_environment
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
    )
    set(vnm_terminal_native_titlebar_environment
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
    )
    if(WIN32)
        list(APPEND vnm_terminal_custom_titlebar_environment
            "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
        list(APPEND vnm_terminal_native_titlebar_environment
            "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
    endif()

    if(vnm_terminal_custom_titlebar_supported)
        add_test(
            NAME vnm_terminal_custom_titlebar_smoke
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_output_regex=vnm_terminal:"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_custom_titlebar_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--window-size" "800x480"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_custom_titlebar_smoke PROPERTIES
            TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_native_titlebar_smoke
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_output_regex=vnm_terminal:"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_native_titlebar_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--native-titlebar"
                "--window-size" "800x480"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_native_titlebar_smoke PROPERTIES
            TIMEOUT 30
        )
    endif()

    add_test(
        NAME vnm_terminal_capture_output
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dcapture_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_capture_output_$<CONFIG>.raw"
            "-Dexpected_capture_regex=terminal-canvas"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_capture_file.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--window-size" "800x480"
            "--capture-output" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_capture_output_$<CONFIG>.raw"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_capture_output PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_metrics_json_smoke
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dmetrics_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_json_$<CONFIG>.json"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_metrics_json.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--window-size" "800x480"
            "--font-size" "10"
            "--metrics-json" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_json_$<CONFIG>.json"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_metrics_json_smoke PROPERTIES
        TIMEOUT 30
    )

    if(VNM_TERMINAL_ENABLE_PROFILING)
        add_test(
            NAME vnm_terminal_metrics_profile_text_smoke
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dmetrics_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_profile_text_$<CONFIG>.json"
                "-Dprofile_text_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_profile_text_$<CONFIG>.txt"
                "-Dexpected_profile_text_requested=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_metrics_json.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--window-size" "800x480"
                "--font-size" "10"
                "--metrics-json" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_profile_text_$<CONFIG>.json"
                "--profile-text" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_metrics_profile_text_$<CONFIG>.txt"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_metrics_profile_text_smoke PROPERTIES
            TIMEOUT 30
        )
    endif()

    if(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
        add_test(
            NAME vnm_terminal_capture_transcript
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dcapture_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_capture_transcript_$<CONFIG>.ndjson"
                "-Dexpected_capture_regex=visible_rows"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_capture_file.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--window-size" "800x480"
                "--capture-transcript" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_capture_transcript_$<CONFIG>.ndjson"
                "--transcript-snapshot-diagnostics"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_capture_transcript PROPERTIES
            TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_rejects_duplicate_capture_paths
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--capture-output and --capture-transcript must use different paths"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--capture-output" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_duplicate_capture_$<CONFIG>.raw"
                "--capture-transcript" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_duplicate_capture_$<CONFIG>.raw"
        )

        set_tests_properties(vnm_terminal_rejects_duplicate_capture_paths PROPERTIES
            TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_invalid_transcript_path_preserves_capture_output
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dpreserved_path=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_preserved_capture_$<CONFIG>.raw"
                "-Dpreserved_text=preserve-output-sentinel"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--capture-transcript parent directory does not exist"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_capture_path_failure_preserves_file.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--capture-output" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_preserved_capture_$<CONFIG>.raw"
                "--capture-transcript" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_preserved_capture_$<CONFIG>.raw/transcript.ndjson"
        )

        set_tests_properties(vnm_terminal_invalid_transcript_path_preserves_capture_output PROPERTIES
            TIMEOUT 30
        )
    else()
        add_test(
            NAME vnm_terminal_rejects_capture_transcript_when_disabled
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--capture-transcript is unavailable because transcript capture/replay is disabled in this build"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--capture-transcript" "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_disabled_transcript_$<CONFIG>.ndjson"
        )

        set_tests_properties(vnm_terminal_rejects_capture_transcript_when_disabled PROPERTIES
            TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_rejects_transcript_snapshot_diagnostics_when_disabled
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--transcript-snapshot-diagnostics is unavailable because transcript capture/replay is disabled in this build"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal>"
                "--transcript-snapshot-diagnostics"
        )

        set_tests_properties(vnm_terminal_rejects_transcript_snapshot_diagnostics_when_disabled PROPERTIES
            TIMEOUT 30
        )
    endif()

    add_test(
        NAME vnm_terminal_preserves_command_args
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--window-size" "800x480"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--echo-argv"
            "-platform"
            "offscreen"
    )

    set_tests_properties(vnm_terminal_preserves_command_args PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_window_size
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--window-size requires <positive-width>x<positive-height>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--window-size" "0x480"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_window_size PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_missing_command
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=explicit command after -- must name a program"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--"
    )

    set_tests_properties(vnm_terminal_rejects_missing_command PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_alternate_wheel
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--alternate-wheel supports only page, cursor, or mouse"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--alternate-wheel" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_alternate_wheel PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_text_renderer
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--text-renderer supports only auto, msdf, or glyph"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--text-renderer" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_text_renderer PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_lcd_subpixel
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--lcd-subpixel supports only auto, none, rgb, bgr, vrgb, or vbgr"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--lcd-subpixel" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_lcd_subpixel PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_osc52_clipboard
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--osc52-clipboard supports only deny or allow"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--osc52-clipboard" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_osc52_clipboard PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_paste_shortcut
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--paste-shortcut supports only disabled, ctrl-shift-v, ctrl-v-and-ctrl-shift-v, or platform-default"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--paste-shortcut" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_paste_shortcut PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_bad_synchronized_output_scroll_policy
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--synchronized-output-scroll-policy supports only defer or immediate-public"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--synchronized-output-scroll-policy" "hidden-live"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_rejects_bad_synchronized_output_scroll_policy PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_rejects_missing_synchronized_output_scroll_policy
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=2"
            "-Dexpected_output_regex=--synchronized-output-scroll-policy requires a value"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--synchronized-output-scroll-policy"
    )

    set_tests_properties(vnm_terminal_rejects_missing_synchronized_output_scroll_policy PROPERTIES
        TIMEOUT 30
    )
endif()

add_test(
    NAME vnm_terminal_help_mentions_alternate_wheel_default
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=alternate-screen wheel: mouse\\(default\\), cursor, or page"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_mentions_alternate_wheel_default PROPERTIES
    TIMEOUT 30
)

add_test(
    NAME vnm_terminal_help_mentions_text_renderer
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=text renderer: auto\\(default\\), msdf, or glyph"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_mentions_text_renderer PROPERTIES
    TIMEOUT 30
)

if(WIN32 AND TARGET vnm_terminal_canvas_fixture)
    add_test(
        NAME vnm_terminal_custom_chrome_starts_from_deployed_runtime
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dforbidden_output_regex=failed to create shared window chrome|Type VNM_ChromeTitleBar unavailable"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "PATH=$<TARGET_FILE_DIR:vnm_terminal>"
            "$<TARGET_FILE:vnm_terminal>"
            "--timeout-ms" "5000"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_custom_chrome_starts_from_deployed_runtime
        PROPERTIES
        TIMEOUT 30
    )
endif()

add_test(
    NAME vnm_terminal_help_mentions_synchronized_output_scroll_policy
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=--synchronized-output-scroll-policy <policy>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

add_test(
    NAME vnm_terminal_help_describes_synchronized_output_scroll_policy_values
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=DEC synchronized-output scroll: defer\\(default\\) or immediate-public \\(case-insensitive\\)"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(
    vnm_terminal_help_mentions_synchronized_output_scroll_policy
    vnm_terminal_help_describes_synchronized_output_scroll_policy_values
    PROPERTIES
    TIMEOUT 30
)

add_test(
    NAME vnm_terminal_help_hides_custom_titlebar
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dforbidden_stdout_regex=--custom-titlebar"
        "-Drequire_empty_stderr=ON"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_hides_custom_titlebar PROPERTIES
    TIMEOUT 30
)

if(vnm_terminal_custom_titlebar_supported)
    add_test(
        NAME vnm_terminal_help_mentions_native_titlebar
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dexpected_stdout_regex=--native-titlebar +use the platform titlebar instead of built-in chrome"
            "-Drequire_empty_stderr=ON"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--help"
    )

    set_tests_properties(vnm_terminal_help_mentions_native_titlebar PROPERTIES
        TIMEOUT 30
    )
endif()

if(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    add_test(
        NAME vnm_terminal_help_mentions_transcript_capture
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dexpected_output_regex=--capture-transcript <path> +write sensitive NDJSON replay transcript"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--help"
    )

    set_tests_properties(vnm_terminal_help_mentions_transcript_capture PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_help_mentions_transcript_snapshot_diagnostics
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dexpected_output_regex=--transcript-snapshot-diagnostics +include visible row text/provenance snapshots"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--help"
    )

    set_tests_properties(vnm_terminal_help_mentions_transcript_snapshot_diagnostics PROPERTIES
        TIMEOUT 30
    )
else()
    add_test(
        NAME vnm_terminal_help_omits_transcript_capture
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dforbidden_stdout_regex=--capture-transcript|--transcript-snapshot-diagnostics"
            "-Drequire_empty_stderr=ON"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/terminal_app/expect_process_clean.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal>"
            "--help"
    )

    set_tests_properties(vnm_terminal_help_omits_transcript_capture PROPERTIES
        TIMEOUT 30
    )
endif()

add_test(
    NAME vnm_terminal_help_mentions_mouse_selection
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=mouse-reporting apps receive unmodified mouse drags; Shift-drag selects locally"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_mentions_mouse_selection PROPERTIES
    TIMEOUT 30
)

add_test(
    NAME vnm_terminal_help_mentions_osc52_policy
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=OSC 52 clipboard writes are denied by default"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_mentions_osc52_policy PROPERTIES
    TIMEOUT 30
)

add_test(
    NAME vnm_terminal_help_mentions_paste_shortcut
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=0"
        "-Dexpected_output_regex=paste shortcut: platform-default\\(default\\), ctrl-shift-v, ctrl-v-and-ctrl-shift-v, or disabled"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal>"
        "--help"
)

set_tests_properties(vnm_terminal_help_mentions_paste_shortcut PROPERTIES
    TIMEOUT 30
)

set(vnm_terminal_distribution_guard_configure_args
    "-G" "${CMAKE_GENERATOR}"
)

if(NOT "${CMAKE_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND vnm_terminal_distribution_guard_configure_args
        "-A" "${CMAKE_GENERATOR_PLATFORM}"
    )
endif()

if(NOT "${CMAKE_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND vnm_terminal_distribution_guard_configure_args
        "-T" "${CMAKE_GENERATOR_TOOLSET}"
    )
endif()

add_test(
    NAME vnm_terminal_rejects_distribution_transcript_build
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dexpected_exit_code=1"
        "-Dexpected_output_regex=VNM_TERMINAL_DISTRIBUTION_BUILD=ON requires"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
        --
        "${CMAKE_COMMAND}"
            ${vnm_terminal_distribution_guard_configure_args}
            -S "${PROJECT_SOURCE_DIR}"
            -B "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_distribution_guard"
            -DVNM_TERMINAL_DISTRIBUTION_BUILD=ON
            -DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON
            -DBUILD_TESTING=OFF
)

set_tests_properties(vnm_terminal_rejects_distribution_transcript_build PROPERTIES
    TIMEOUT 30
)

set(vnm_terminal_test_process_targets
    vnm_terminal_title_metadata
    vnm_terminal_custom_titlebar_integration
    vnm_terminal_app_persistence
)

foreach(vnm_terminal_test_process_target IN LISTS vnm_terminal_test_process_targets)
    vnm_terminal_configure_test_process(${vnm_terminal_test_process_target})
endforeach()
