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
find_package(Threads REQUIRED)

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

set(vnm_terminal_qt_scaled_environment
    "QT_SCALE_FACTOR=1.5"
    ${vnm_terminal_qt_scale_environment}
)

set(vnm_terminal_example_terminal_custom_titlebar_supported OFF)
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL Linux)
    set(vnm_terminal_example_terminal_custom_titlebar_supported ON)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(vnm_terminal_default_capture_replay_dir "")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/conformance/captures")
    set(vnm_terminal_default_capture_replay_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/conformance/captures")
endif()

set(VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR "" CACHE PATH
    "Unicode data directory used by vnm_terminal_unicode_data_conformance.")
set(VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE OFF CACHE BOOL
    "Add optional Unicode conformance checks for full GraphemeBreakTest and emoji ZWJ sequences.")
set(VNM_TERMINAL_CAPTURE_REPLAY_DIR "${vnm_terminal_default_capture_replay_dir}" CACHE PATH
    "Directory of local .raw/.vnm_capture captures for replay conformance.")
set(VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR "" CACHE PATH
    "Directory of graphics-protocol captures for replay conformance.")
set(VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR "${vnm_terminal_default_capture_replay_dir}" CACHE PATH
    "Directory of seed files for parser fuzz-smoke replay.")
set(VNM_TERMINAL_BUILD_LIBFUZZER OFF CACHE BOOL
    "Build the parser libFuzzer harness target when the compiler supports it.")
set(VNM_TERMINAL_LIBVTERM_DIFF_COMMAND "" CACHE STRING
    "Semicolon-separated external libvterm differential command.")
set(VNM_TERMINAL_LIBVTERM_INCLUDE_DIR "" CACHE PATH
    "libvterm include directory for the optional linked differential harness.")
set(VNM_TERMINAL_LIBVTERM_LIBRARY "" CACHE FILEPATH
    "libvterm library path for the optional linked differential harness.")
set(VNM_TERMINAL_LIBVTERM_CORPUS_DIR "" CACHE PATH
    "Directory of .raw/.vnm_capture files for the linked libvterm differential harness.")
set(VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND "" CACHE STRING
    "Semicolon-separated external graphics conformance command.")
set(VNM_TERMINAL_VTTEST_COMMAND "" CACHE STRING
    "Semicolon-separated vttest command for the manual launch target.")
set(VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT "300" CACHE STRING
    "CTest timeout, in seconds, for external conformance commands.")
set(VNM_TERMINAL_ENABLE_MANUAL_QSG_FLAT_RECT_BATCHED_GEOMETRY_TEST OFF CACHE BOOL
    "Add the manual QSG flat-rect batched-geometry CTest for non-software scene graph hosts.")

function(vnm_terminal_configure_test_process target_name)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /FIvnm_terminal_test_process.h
            /FS
        )
    endif()
endfunction()

function(vnm_terminal_add_external_conformance_command test_name labels command_value)
    if("${command_value}" STREQUAL "")
        return()
    endif()

    set(vnm_terminal_external_command ${command_value})
    add_test(
        NAME ${test_name}
        COMMAND ${vnm_terminal_external_command}
    )

    set_tests_properties(${test_name} PROPERTIES
        LABELS "${labels}"
        TIMEOUT "${VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT}"
    )
endfunction()

add_executable(vnm_terminal_profiler
    profiler/hierarchical_profiler_tests.cpp
)

target_include_directories(vnm_terminal_profiler PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_compile_definitions(vnm_terminal_profiler PRIVATE
    VNM_TERMINAL_PROFILING_ENABLED=1
)

target_link_libraries(vnm_terminal_profiler PRIVATE
    Threads::Threads
)

set_target_properties(vnm_terminal_profiler PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_profiler
    COMMAND
        "$<TARGET_FILE:vnm_terminal_profiler>"
)

add_executable(vnm_terminal_parser_ir
    parser_ir/parser_ir_tests.cpp
)

target_link_libraries(vnm_terminal_parser_ir PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_parser_ir PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_parser_ir
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_parser_ir>"
)

add_executable(vnm_terminal_unicode_width
    unicode_width/unicode_width_tests.cpp
)

target_link_libraries(vnm_terminal_unicode_width PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_unicode_width PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_unicode_width
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_unicode_width>"
)

add_test(
    NAME vnm_terminal_unicode_width_generator
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/unicode_width/generate_unicode_width_tables.py"
        --self-test
)

add_executable(vnm_terminal_unicode_width_tables
    unicode_width_tables/unicode_width_tables_tests.cpp
)

target_link_libraries(vnm_terminal_unicode_width_tables PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_unicode_width_tables PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_unicode_width_tables
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_unicode_width_tables>"
)

add_executable(vnm_terminal_sequence_matrix
    sequence_matrix/sequence_matrix_tests.cpp
)

set_target_properties(vnm_terminal_sequence_matrix PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_sequence_matrix
    COMMAND
        "$<TARGET_FILE:vnm_terminal_sequence_matrix>"
        "${CMAKE_SOURCE_DIR}/docs/terminal_sequence_matrix.md"
        "${CMAKE_SOURCE_DIR}/docs/terminal_conformance_oracles.md"
)

add_executable(vnm_terminal_canvas_fixture_contract
    terminal_canvas_fixture/terminal_canvas_fixture_tests.cpp
)

add_dependencies(vnm_terminal_canvas_fixture_contract
    vnm_terminal_canvas_fixture
)

target_include_directories(vnm_terminal_canvas_fixture_contract PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(vnm_terminal_canvas_fixture_contract PRIVATE
    Qt6::Core
)

set_target_properties(vnm_terminal_canvas_fixture_contract PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_canvas_fixture_contract
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture_contract>"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
)

add_executable(vnm_terminal_qt_render_smoke
    qt_render_smoke/render_smoke_tests.cpp
)

target_link_libraries(vnm_terminal_qt_render_smoke PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_qt_render_smoke PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_render_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_render_smoke>"
)

add_executable(vnm_terminal_qt_metrics
    qt_metrics/qt_metrics_tests.cpp
)

target_link_libraries(vnm_terminal_qt_metrics PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_qt_metrics PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_metrics
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_metrics>"
        "--expect-normal"
)

add_test(
    NAME vnm_terminal_qt_metrics_scaled
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_scaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_metrics>"
        "--expect-scaled"
)

add_executable(vnm_terminal_render_snapshot
    render_snapshot/render_snapshot_tests.cpp
)

target_link_libraries(vnm_terminal_render_snapshot PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_render_snapshot PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_render_snapshot
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_render_snapshot>"
)

add_executable(vnm_terminal_render_frame
    render_frame/render_frame_tests.cpp
)

target_link_libraries(vnm_terminal_render_frame PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_render_frame PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_render_frame
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_render_frame>"
)

add_executable(vnm_terminal_qsg_render
    qsg_render/qsg_render_tests.cpp
)

target_link_libraries(vnm_terminal_qsg_render PRIVATE
    vnm_terminal::vnm_terminal
)

file(GLOB vnm_terminal_qsg_renderer_source_files CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/qsg*.cpp"
)
list(JOIN vnm_terminal_qsg_renderer_source_files "|" vnm_terminal_qsg_renderer_source_paths)

target_compile_definitions(vnm_terminal_qsg_render PRIVATE
    VNM_TERMINAL_QSG_RENDERER_SOURCE_PATHS="${vnm_terminal_qsg_renderer_source_paths}"
)

set_target_properties(vnm_terminal_qsg_render PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qsg_render
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qsg_render>"
        "--software-renderer"
)

set_tests_properties(vnm_terminal_qsg_render PROPERTIES TIMEOUT 60)

if(VNM_TERMINAL_ENABLE_MANUAL_QSG_FLAT_RECT_BATCHED_GEOMETRY_TEST)
    add_test(
        NAME vnm_terminal_qsg_render_flat_rect_batched_geometry
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_render>"
            "--flat-rect-batched-geometry"
    )

    set_tests_properties(vnm_terminal_qsg_render_flat_rect_batched_geometry PROPERTIES
        LABELS "manual;qsg;hardware"
        TIMEOUT 60
    )
endif()

add_executable(vnm_terminal_shaping_contract
    shaping_contract/shaping_contract_tests.cpp
)

target_link_libraries(vnm_terminal_shaping_contract PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_shaping_contract PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_shaping_contract
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_shaping_contract>"
)

add_executable(vnm_terminal_qsg_text_node
    qsg_text_node/qsg_text_node_tests.cpp
)

target_link_libraries(vnm_terminal_qsg_text_node PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

target_compile_definitions(vnm_terminal_qsg_text_node PRIVATE
    VNM_TERMINAL_TEST_MONOSPACE_FONT_PATH="${PROJECT_SOURCE_DIR}/resources/fonts/vnm_framework_monospace.ttf"
)

set_target_properties(vnm_terminal_qsg_text_node PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qsg_text_node
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qsg_text_node>"
        "--software-renderer"
)
set_tests_properties(vnm_terminal_qsg_text_node PROPERTIES
    TIMEOUT 60
)

add_executable(vnm_terminal_surface_host
    surface_host/surface_host_tests.cpp
)

target_link_libraries(vnm_terminal_surface_host PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_surface_host PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_surface_host
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_surface_host>"
)

set_tests_properties(vnm_terminal_surface_host PROPERTIES
    LABELS "conformance;input;mouse;paste;focus"
    TIMEOUT 60
)

add_executable(vnm_terminal_input_encoder
    input_encoder/input_encoder_tests.cpp
)

target_link_libraries(vnm_terminal_input_encoder PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_input_encoder PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_input_encoder
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_input_encoder>"
)

set_tests_properties(vnm_terminal_input_encoder PROPERTIES
    LABELS "conformance;input;mouse;paste;focus"
    TIMEOUT 60
)

add_executable(vnm_terminal_behavior_smoke
    behavior_smoke/behavior_smoke_tests.cpp
)

add_dependencies(vnm_terminal_behavior_smoke
    vnm_terminal_canvas_fixture
)

target_link_libraries(vnm_terminal_behavior_smoke PRIVATE
    vnm_terminal::vnm_terminal
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

set_target_properties(vnm_terminal_behavior_smoke PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_behavior_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
)

set_tests_properties(vnm_terminal_behavior_smoke PROPERTIES
    TIMEOUT 60
)

add_test(
    NAME vnm_terminal_surface_behavior_smokes
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
        "--surface-host"
)

set_tests_properties(vnm_terminal_surface_behavior_smokes PROPERTIES
    TIMEOUT 60
)

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL Linux)
    add_test(
        NAME vnm_terminal_native_surface_behavior_smokes
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
            "--native-surface-host"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_native_surface_behavior_smokes PROPERTIES
        TIMEOUT 120
    )
endif()

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL Linux)
    add_executable(vnm_terminal_compat_smoke
        compat_smoke/compat_smoke_tests.cpp
    )

    add_dependencies(vnm_terminal_compat_smoke
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_compat_smoke PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_compat_smoke PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_compat_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_compat_smoke>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_compat_smoke PROPERTIES
        TIMEOUT 120
    )
endif()

add_executable(vnm_terminal_parser_randomized
    parser_randomized/parser_randomized_tests.cpp
)

target_link_libraries(vnm_terminal_parser_randomized PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_parser_randomized PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_parser_randomized
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_parser_randomized>"
        "${CMAKE_CURRENT_SOURCE_DIR}/parser_randomized"
)

set_tests_properties(vnm_terminal_parser_randomized PROPERTIES
    LABELS "conformance;fuzz;parser"
    TIMEOUT 60
)

add_executable(vnm_terminal_resource_lifecycle
    resource_lifecycle/resource_lifecycle_tests.cpp
)

target_link_libraries(vnm_terminal_resource_lifecycle PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_resource_lifecycle PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_resource_lifecycle
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_resource_lifecycle>"
)

set_tests_properties(vnm_terminal_resource_lifecycle PROPERTIES
    TIMEOUT 60
)

if(NOT VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR STREQUAL "")
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR is not a directory: "
            "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}")
    endif()

    add_executable(vnm_terminal_unicode_data_conformance
        conformance/unicode_data_conformance_tests.cpp
    )

    target_link_libraries(vnm_terminal_unicode_data_conformance PRIVATE
        vnm_terminal::vnm_terminal
        Qt6::Core
    )

    set_target_properties(vnm_terminal_unicode_data_conformance PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_unicode_data_conformance
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_unicode_data_conformance>"
            "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}"
    )

    set_tests_properties(vnm_terminal_unicode_data_conformance PROPERTIES
        LABELS "conformance;unicode"
        TIMEOUT 120
    )

    if(VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE)
        add_test(
            NAME vnm_terminal_unicode_data_aspirational_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_unicode_data_conformance>"
                "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}"
                "--include-aspirational"
        )

        set_tests_properties(vnm_terminal_unicode_data_aspirational_conformance PROPERTIES
            LABELS "conformance;unicode;aspirational"
            TIMEOUT 120
        )
    endif()
endif()

if((NOT VNM_TERMINAL_CAPTURE_REPLAY_DIR STREQUAL "") OR
    (NOT VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR STREQUAL ""))
    add_executable(vnm_terminal_capture_replay_conformance
        conformance/capture_replay_conformance_tests.cpp
    )

    target_link_libraries(vnm_terminal_capture_replay_conformance PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_capture_replay_conformance PROPERTIES
        AUTOMOC OFF
    )

    if(NOT VNM_TERMINAL_CAPTURE_REPLAY_DIR STREQUAL "")
        if(NOT IS_DIRECTORY "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}")
            message(FATAL_ERROR
                "VNM_TERMINAL_CAPTURE_REPLAY_DIR is not a directory: "
                "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}")
        endif()

        add_test(
            NAME vnm_terminal_capture_replay_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_capture_replay_conformance>"
                "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}"
        )

        set_tests_properties(vnm_terminal_capture_replay_conformance PROPERTIES
            LABELS "conformance;capture-replay"
            TIMEOUT 180
        )
    endif()

    if(NOT VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR STREQUAL "")
        if(NOT IS_DIRECTORY "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}")
            message(FATAL_ERROR
                "VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR is not a directory: "
                "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}")
        endif()

        add_test(
            NAME vnm_terminal_graphics_capture_replay_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_capture_replay_conformance>"
                "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}"
        )

        set_tests_properties(vnm_terminal_graphics_capture_replay_conformance PROPERTIES
            LABELS "conformance;graphics;capture-replay"
            TIMEOUT 180
        )
    endif()
endif()

if(NOT VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR STREQUAL "")
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR is not a directory: "
            "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}")
    endif()

    add_executable(vnm_terminal_parser_fuzz_smoke
        conformance/parser_libfuzzer_harness.cpp
    )

    target_link_libraries(vnm_terminal_parser_fuzz_smoke PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_parser_fuzz_smoke PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_parser_fuzz_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_parser_fuzz_smoke>"
            "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}"
    )

    set_tests_properties(vnm_terminal_parser_fuzz_smoke PROPERTIES
        LABELS "conformance;fuzz;parser"
        TIMEOUT 120
    )
endif()

if((NOT VNM_TERMINAL_LIBVTERM_INCLUDE_DIR STREQUAL "") OR
    (NOT VNM_TERMINAL_LIBVTERM_LIBRARY STREQUAL "") OR
    (NOT VNM_TERMINAL_LIBVTERM_CORPUS_DIR STREQUAL ""))
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_INCLUDE_DIR is not a directory: "
            "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${VNM_TERMINAL_LIBVTERM_LIBRARY}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_LIBRARY does not exist: "
            "${VNM_TERMINAL_LIBVTERM_LIBRARY}")
    endif()
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_CORPUS_DIR is not a directory: "
            "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}")
    endif()

    add_executable(vnm_terminal_libvterm_differential
        conformance/libvterm_differential_tests.cpp
    )

    target_include_directories(vnm_terminal_libvterm_differential PRIVATE
        "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}"
    )

    target_link_libraries(vnm_terminal_libvterm_differential PRIVATE
        vnm_terminal::vnm_terminal
        "${VNM_TERMINAL_LIBVTERM_LIBRARY}"
    )

    set_target_properties(vnm_terminal_libvterm_differential PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_libvterm_differential
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_libvterm_differential>"
            "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}"
    )

    set_tests_properties(vnm_terminal_libvterm_differential PROPERTIES
        LABELS "conformance;libvterm"
        TIMEOUT 180
    )
endif()

if(VNM_TERMINAL_BUILD_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "VNM_TERMINAL_BUILD_LIBFUZZER requires a Clang compiler")
    endif()

    add_executable(vnm_terminal_parser_libfuzzer
        conformance/parser_libfuzzer_harness.cpp
    )

    target_compile_definitions(vnm_terminal_parser_libfuzzer PRIVATE
        VNM_TERMINAL_LIBFUZZER
    )

    target_compile_options(vnm_terminal_parser_libfuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )

    target_link_options(vnm_terminal_parser_libfuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )

    target_link_libraries(vnm_terminal_parser_libfuzzer PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_parser_libfuzzer PROPERTIES
        AUTOMOC OFF
    )
endif()

vnm_terminal_add_external_conformance_command(
    vnm_terminal_external_libvterm_diff
    "conformance;libvterm;external"
    "${VNM_TERMINAL_LIBVTERM_DIFF_COMMAND}")

vnm_terminal_add_external_conformance_command(
    vnm_terminal_external_graphics_conformance
    "conformance;graphics;external"
    "${VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND}")

if(TARGET vnm_terminal_example_terminal)
    add_executable(vnm_terminal_example_terminal_window_chrome_model
        example_terminal/window_chrome_model_tests.cpp
    )

    target_include_directories(vnm_terminal_example_terminal_window_chrome_model PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/terminal_app"
    )

    target_link_libraries(vnm_terminal_example_terminal_window_chrome_model PRIVATE
        Qt6::Core
    )

    set_target_properties(vnm_terminal_example_terminal_window_chrome_model PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_example_terminal_window_chrome_model
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal_window_chrome_model>"
    )

    set_tests_properties(vnm_terminal_example_terminal_window_chrome_model PROPERTIES
        TIMEOUT 30
    )

    add_executable(vnm_terminal_example_terminal_window_chrome_item
        example_terminal/window_chrome_item_tests.cpp
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/window_chrome.cpp"
    )

    qt_add_resources(vnm_terminal_example_terminal_window_chrome_item
        "vnm_terminal_example_terminal_window_chrome_item_resources"
        PREFIX
            "/vnm_terminal_example_terminal"
        BASE
            "${PROJECT_SOURCE_DIR}/examples/terminal_app"
        FILES
            "${PROJECT_SOURCE_DIR}/examples/terminal_app/vnm_terminal.ico"
    )

    target_include_directories(vnm_terminal_example_terminal_window_chrome_item PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/terminal_app"
    )

    target_link_libraries(vnm_terminal_example_terminal_window_chrome_item PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    )

    set_target_properties(vnm_terminal_example_terminal_window_chrome_item PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_example_terminal_window_chrome_item
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal_window_chrome_item>"
    )

    set_tests_properties(vnm_terminal_example_terminal_window_chrome_item PROPERTIES
        TIMEOUT 30
    )

    add_executable(vnm_terminal_example_terminal_window_chrome_resize_filter
        example_terminal/window_chrome_resize_filter_tests.cpp
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/frameless_resize_filter.cpp"
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/window_chrome.cpp"
    )

    target_include_directories(vnm_terminal_example_terminal_window_chrome_resize_filter PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/terminal_app"
    )

    target_link_libraries(vnm_terminal_example_terminal_window_chrome_resize_filter PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    )

    set_target_properties(vnm_terminal_example_terminal_window_chrome_resize_filter PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_example_terminal_window_chrome_resize_filter
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal_window_chrome_resize_filter>"
    )

    set_tests_properties(vnm_terminal_example_terminal_window_chrome_resize_filter PROPERTIES
        TIMEOUT 30
    )

    add_executable(vnm_terminal_example_terminal_custom_titlebar_integration
        example_terminal/custom_titlebar_integration_tests.cpp
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/frameless_resize_filter.cpp"
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/terminal_scrollbar.cpp"
        "${PROJECT_SOURCE_DIR}/examples/terminal_app/window_chrome.cpp"
    )

    target_include_directories(vnm_terminal_example_terminal_custom_titlebar_integration PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/terminal_app"
    )

    target_link_libraries(vnm_terminal_example_terminal_custom_titlebar_integration PRIVATE
        vnm_terminal::vnm_terminal
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    )

    set_target_properties(vnm_terminal_example_terminal_custom_titlebar_integration PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_example_terminal_custom_titlebar_integration
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal_custom_titlebar_integration>"
    )

    set_tests_properties(vnm_terminal_example_terminal_custom_titlebar_integration PROPERTIES
        TIMEOUT 30
    )

    add_dependencies(vnm_terminal_behavior_smoke
        vnm_terminal_example_terminal
    )

    add_test(
        NAME vnm_terminal_example_terminal_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--window-size" "800x480"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_example_terminal_smoke PROPERTIES
        TIMEOUT 30
    )

    set(vnm_terminal_example_terminal_custom_titlebar_environment
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
    )
    set(vnm_terminal_example_terminal_native_titlebar_environment
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
    )
    if(WIN32)
        list(APPEND vnm_terminal_example_terminal_custom_titlebar_environment
            "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
        list(APPEND vnm_terminal_example_terminal_native_titlebar_environment
            "QT_QPA_FONTDIR=$ENV{SystemRoot}/Fonts")
    endif()

    if(vnm_terminal_example_terminal_custom_titlebar_supported)
        add_test(
            NAME vnm_terminal_example_terminal_custom_titlebar_smoke
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_output_regex=vnm_terminal_example_terminal:"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/example_terminal/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_example_terminal_custom_titlebar_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--window-size" "800x480"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_example_terminal_custom_titlebar_smoke PROPERTIES
            TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_example_terminal_native_titlebar_smoke
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_output_regex=vnm_terminal_example_terminal:"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/example_terminal/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_example_terminal_native_titlebar_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--native-titlebar"
                "--window-size" "800x480"
                "--timeout-ms" "10000"
                "--require-output"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_example_terminal_native_titlebar_smoke PROPERTIES
            TIMEOUT 30
        )
    endif()

    add_test(
        NAME vnm_terminal_example_terminal_capture_output
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dcapture_path=${CMAKE_CURRENT_BINARY_DIR}/example_terminal_capture_output_$<CONFIG>.raw"
            "-Dexpected_capture_regex=terminal-canvas"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_capture_file.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--window-size" "800x480"
            "--capture-output" "${CMAKE_CURRENT_BINARY_DIR}/example_terminal_capture_output_$<CONFIG>.raw"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_example_terminal_capture_output PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_preserves_command_args
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--window-size" "800x480"
            "--timeout-ms" "10000"
            "--require-output"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--echo-argv"
            "-platform"
            "offscreen"
    )

    set_tests_properties(vnm_terminal_example_terminal_preserves_command_args PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_rejects_bad_window_size
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
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--window-size" "0x480"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_example_terminal_rejects_bad_window_size PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_rejects_missing_command
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
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--"
    )

    set_tests_properties(vnm_terminal_example_terminal_rejects_missing_command PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_rejects_bad_alternate_wheel
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
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--alternate-wheel" "invalid"
            "--"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "--list"
    )

    set_tests_properties(vnm_terminal_example_terminal_rejects_bad_alternate_wheel PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_help_mentions_alternate_wheel_default
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
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--help"
    )

    set_tests_properties(vnm_terminal_example_terminal_help_mentions_alternate_wheel_default
        PROPERTIES
            TIMEOUT 30
    )

    if(vnm_terminal_example_terminal_custom_titlebar_supported)
        add_test(
            NAME vnm_terminal_example_terminal_help_hides_custom_titlebar
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_stdout_regex=--custom-titlebar"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/example_terminal/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--help"
        )

        set_tests_properties(vnm_terminal_example_terminal_help_hides_custom_titlebar
            PROPERTIES
                TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_example_terminal_help_mentions_native_titlebar
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dexpected_stdout_regex=--native-titlebar +use the platform titlebar instead of built-in chrome"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/example_terminal/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--help"
        )

        set_tests_properties(vnm_terminal_example_terminal_help_mentions_native_titlebar
            PROPERTIES
                TIMEOUT 30
        )
    else()
        add_test(
            NAME vnm_terminal_example_terminal_help_hides_custom_titlebar
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=0"
                "-Dforbidden_stdout_regex=--(custom|native)-titlebar"
                "-Drequire_empty_stderr=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/example_terminal/expect_process_clean.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--help"
        )

        set_tests_properties(vnm_terminal_example_terminal_help_hides_custom_titlebar
            PROPERTIES
                TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_example_terminal_rejects_custom_titlebar
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--custom-titlebar is supported only after platform validation"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--custom-titlebar"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_example_terminal_rejects_custom_titlebar
            PROPERTIES
                TIMEOUT 30
        )

        add_test(
            NAME vnm_terminal_example_terminal_rejects_native_titlebar
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dexpected_exit_code=2"
                "-Dexpected_output_regex=--native-titlebar is supported only after platform validation"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
                --
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_offscreen_environment}
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--native-titlebar"
                "--"
                "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
                "--list"
        )

        set_tests_properties(vnm_terminal_example_terminal_rejects_native_titlebar
            PROPERTIES
                TIMEOUT 30
        )
    endif()

    add_test(
        NAME vnm_terminal_example_terminal_help_mentions_mouse_selection
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
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--help"
    )

    set_tests_properties(vnm_terminal_example_terminal_help_mentions_mouse_selection PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_help_mentions_osc52_policy
        COMMAND
            "${CMAKE_COMMAND}"
            "-Dexpected_exit_code=0"
            "-Dexpected_output_regex=OSC 52 clipboard writes are allowed for target c/clipboard"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_result.cmake"
            --
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
            "--software-renderer"
            "--help"
    )

    set_tests_properties(vnm_terminal_example_terminal_help_mentions_osc52_policy PROPERTIES
        TIMEOUT 30
    )

    add_test(
        NAME vnm_terminal_example_terminal_behavior_smoke_launches
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
            "--host"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
            "$<TARGET_FILE:vnm_terminal_example_terminal>"
    )

    set_tests_properties(vnm_terminal_example_terminal_behavior_smoke_launches PROPERTIES
        TIMEOUT 240
    )

    if(NOT VNM_TERMINAL_VTTEST_COMMAND STREQUAL "")
        set(vnm_terminal_vttest_command ${VNM_TERMINAL_VTTEST_COMMAND})
        add_custom_target(vnm_terminal_external_vttest
            COMMAND
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_example_terminal>"
                "--software-renderer"
                "--window-size" "800x480"
                "--"
                ${vnm_terminal_vttest_command}
            USES_TERMINAL
        )
    endif()
endif()

add_executable(vnm_terminal_qt_posture
    qt_posture/qt_posture_tests.cpp
)

set_target_properties(vnm_terminal_qt_posture PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_posture
    COMMAND
        "$<TARGET_FILE:vnm_terminal_qt_posture>"
        "${CMAKE_COMMAND}"
        "${CMAKE_GENERATOR}"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/qt_posture_cases"
)

add_executable(vnm_terminal_screen_basic
    screen_basic/basic_model_tests.cpp
)

target_link_libraries(vnm_terminal_screen_basic PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_screen_basic PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_basic
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_basic>"
)

add_executable(vnm_terminal_screen_sgr
    screen_sgr/sgr_model_tests.cpp
)

target_link_libraries(vnm_terminal_screen_sgr PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_screen_sgr PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_sgr
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_sgr>"
)

add_executable(vnm_terminal_screen_operations
    screen_operations/model_ops_tests.cpp
)

target_link_libraries(vnm_terminal_screen_operations PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_screen_operations PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_operations
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_operations>"
)

add_executable(vnm_terminal_screen_alternate
    screen_alternate/alternate_screen_tests.cpp
)

target_link_libraries(vnm_terminal_screen_alternate PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_screen_alternate PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_alternate
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_alternate>"
)

add_executable(vnm_terminal_terminal_modes
    terminal_modes/private_modes_tests.cpp
)

target_link_libraries(vnm_terminal_terminal_modes PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_terminal_modes PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_terminal_modes
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_terminal_modes>"
)

add_executable(vnm_terminal_viewport
    viewport/viewport_controller_tests.cpp
)

target_link_libraries(vnm_terminal_viewport PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_viewport PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_viewport
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_viewport>"
)

add_executable(vnm_terminal_backend_session
    backend_session/backend_session_tests.cpp
)

target_link_libraries(vnm_terminal_backend_session PRIVATE
    vnm_terminal::vnm_terminal
)

set_target_properties(vnm_terminal_backend_session PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_backend_session
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_backend_session>"
)

if(WIN32)
    add_executable(vnm_terminal_windows_conpty_backend
        windows_conpty_backend/windows_conpty_backend_tests.cpp
    )

    add_dependencies(vnm_terminal_windows_conpty_backend
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_windows_conpty_backend PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_windows_conpty_backend PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_windows_conpty_backend
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_windows_conpty_backend>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_windows_conpty_backend PROPERTIES
        LABELS "conformance;windows;conpty"
        TIMEOUT 120
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    add_executable(vnm_terminal_linux_pty_backend
        linux_pty_backend/linux_pty_backend_tests.cpp
    )

    add_dependencies(vnm_terminal_linux_pty_backend
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_linux_pty_backend PRIVATE
        vnm_terminal::vnm_terminal
    )

    set_target_properties(vnm_terminal_linux_pty_backend PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_linux_pty_backend
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_linux_pty_backend>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_linux_pty_backend PROPERTIES
        TIMEOUT 120
    )
endif()

set(vnm_terminal_test_process_targets
    vnm_terminal_canvas_fixture
    vnm_terminal_profiler
    vnm_terminal_parser_ir
    vnm_terminal_unicode_width
    vnm_terminal_unicode_width_tables
    vnm_terminal_sequence_matrix
    vnm_terminal_canvas_fixture_contract
    vnm_terminal_qt_render_smoke
    vnm_terminal_qt_metrics
    vnm_terminal_render_snapshot
    vnm_terminal_render_frame
    vnm_terminal_qsg_render
    vnm_terminal_shaping_contract
    vnm_terminal_qsg_text_node
    vnm_terminal_input_encoder
    vnm_terminal_surface_host
    vnm_terminal_behavior_smoke
    vnm_terminal_parser_randomized
    vnm_terminal_resource_lifecycle
    vnm_terminal_qt_posture
    vnm_terminal_screen_basic
    vnm_terminal_screen_sgr
    vnm_terminal_screen_operations
    vnm_terminal_screen_alternate
    vnm_terminal_terminal_modes
    vnm_terminal_viewport
    vnm_terminal_backend_session
)

if(WIN32)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_windows_conpty_backend
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_linux_pty_backend
    )
endif()

if(TARGET vnm_terminal_compat_smoke)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_compat_smoke
    )
endif()

if(TARGET vnm_terminal_unicode_data_conformance)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_unicode_data_conformance
    )
endif()

if(TARGET vnm_terminal_capture_replay_conformance)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_capture_replay_conformance
    )
endif()

if(TARGET vnm_terminal_parser_fuzz_smoke)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_parser_fuzz_smoke
    )
endif()

if(TARGET vnm_terminal_libvterm_differential)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_libvterm_differential
    )
endif()

if(TARGET vnm_terminal_parser_libfuzzer)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_parser_libfuzzer
    )
endif()

if(TARGET vnm_terminal_example_terminal)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_example_terminal
    )
endif()

if(TARGET vnm_terminal_example_terminal_window_chrome_model)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_example_terminal_window_chrome_model
    )
endif()

if(TARGET vnm_terminal_example_terminal_window_chrome_item)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_example_terminal_window_chrome_item
    )
endif()

if(TARGET vnm_terminal_example_terminal_window_chrome_resize_filter)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_example_terminal_window_chrome_resize_filter
    )
endif()

if(TARGET vnm_terminal_example_terminal_custom_titlebar_integration)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_example_terminal_custom_titlebar_integration
    )
endif()

foreach(vnm_terminal_test_process_target IN LISTS vnm_terminal_test_process_targets)
    vnm_terminal_configure_test_process(${vnm_terminal_test_process_target})
endforeach()
