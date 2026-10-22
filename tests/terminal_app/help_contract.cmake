# Capture one invocation so every help assertion observes the same output.
set(expected_exit_code 0)
set(require_empty_stderr ON)
include("${CMAKE_CURRENT_LIST_DIR}/expect_process_clean.cmake")

function(require_help pattern)
    if(NOT stdout_text MATCHES "${pattern}")
        message(FATAL_ERROR
            "expected help stdout to match ${pattern}\n"
            "stdout:\n${stdout_text}")
    endif()
endfunction()

require_help([[alternate-screen wheel: mouse\(default\), cursor, or page]])
require_help([[text renderer: auto\(default\), msdf, or glyph]])
require_help([[row hover timestamp tooltip: on\(default\) or off]])
require_help([[--synchronized-output-scroll-policy <policy>]])
require_help([[DEC synchronized-output scroll: defer\(default\) or immediate-public \(case-insensitive\)]])
require_help([[mouse-reporting apps receive unmodified mouse drags; Shift-drag selects locally]])
require_help([[OSC 52 clipboard writes are denied by default]])
require_help([[paste shortcut: platform-default\(default\), ctrl-shift-v, ctrl-v-and-ctrl-shift-v, or disabled]])

if(native_titlebar_supported)
    require_help([[--native-titlebar +use the platform titlebar instead of built-in chrome]])
endif()

if(transcript_capture_replay_enabled)
    require_help([[--capture-transcript <path> +write sensitive NDJSON replay transcript]])
    require_help([[--transcript-snapshot-diagnostics +include visible row text/provenance snapshots]])
elseif(stdout_text MATCHES "--capture-transcript|--transcript-snapshot-diagnostics")
    message(FATAL_ERROR
        "transcript options must be absent when capture/replay is disabled\n"
        "stdout:\n${stdout_text}")
endif()
