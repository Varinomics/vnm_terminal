cmake_minimum_required(VERSION 3.21)
project(vnm_terminal LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(VNM_TERMINAL_BUILD_EXAMPLES "Build vnm_terminal example applications" ON)
option(VNM_TERMINAL_BUILD_REQUIRED_READINESS "Build vnm_terminal readiness validation gates" OFF)
option(VNM_TERMINAL_BUILD_BENCHMARKS "Build vnm_terminal benchmark executables" OFF)
option(VNM_TERMINAL_ENABLE_PROFILING "Compile vnm_terminal profile scope instrumentation" OFF)

if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT VNM_TERMINAL_BUILD_BENCHMARKS)
    set(VNM_TERMINAL_BUILD_BENCHMARKS ON)
endif()
if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT VNM_TERMINAL_ENABLE_PROFILING)
    set(VNM_TERMINAL_ENABLE_PROFILING ON)
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Quick)
include(cmake/vnm_terminal_qt_posture.cmake)

set(vnm_terminal_qt_targets
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

vnm_terminal_validate_qt_posture(DIRECT_TARGETS
    ${vnm_terminal_qt_targets}
)

add_library(vnm_terminal STATIC
    include/vnm_terminal/internal/backend_contract.h
    include/vnm_terminal/internal/ime_contract.h
    include/vnm_terminal/internal/metrics_contract.h
    include/vnm_terminal/internal/parser_action.h
    include/vnm_terminal/internal/qsg_terminal_render_frame.h
    include/vnm_terminal/internal/qsg_terminal_renderer.h
    include/vnm_terminal/internal/qt_grid_metrics_provider.h
    include/vnm_terminal/internal/render_snapshot.h
    include/vnm_terminal/internal/selection_contract.h
    include/vnm_terminal/internal/session_contract.h
    include/vnm_terminal/internal/terminal_canvas_fixture_contract.h
    include/vnm_terminal/internal/terminal_input_mode.h
    include/vnm_terminal/internal/terminal_input_encoder.h
    include/vnm_terminal/internal/terminal_resize_controller.h
    include/vnm_terminal/internal/terminal_screen_model.h
    include/vnm_terminal/internal/terminal_session.h
    include/vnm_terminal/internal/terminal_style.h
    include/vnm_terminal/internal/utf8_scan.h
    include/vnm_terminal/internal/unicode_width.h
    include/vnm_terminal/internal/unicode_width_tables.h
    include/vnm_terminal/internal/vnm_terminal_font.h
    include/vnm_terminal/internal/viewport_contract.h
    include/vnm_terminal/internal/vnm_terminal_surface_render_bridge.h
    include/vnm_terminal/internal/linux_pty_backend.h
    include/vnm_terminal/internal/windows_conpty_backend.h
    include/vnm_terminal/vnm_terminal_surface.h
    src/terminal_input_encoder.cpp
    src/terminal_resize_controller.cpp
    src/terminal_session.cpp
    src/terminal_screen_model.cpp
    src/metrics_contract.cpp
    src/native_backend_io_core.cpp
    src/qsg_terminal_renderer.cpp
    src/qt_grid_metrics_provider.cpp
    src/selection_contract.cpp
    src/session_contract.cpp
    src/unicode_width_tables.cpp
    src/viewport_contract.cpp
    src/vnm_terminal_font.cpp
    src/vnm_terminal_surface.cpp
)

qt_add_resources(vnm_terminal "vnm_terminal_resources"
    PREFIX
        "/vnm_terminal"
    BASE
        "resources"
    FILES
        resources/fonts/vnm_framework_monospace.ttf
)

if(WIN32)
    target_sources(vnm_terminal PRIVATE
        src/windows_conpty_backend.cpp
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    target_sources(vnm_terminal PRIVATE
        src/linux_pty_backend.cpp
    )
endif()

target_include_directories(vnm_terminal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(vnm_terminal PUBLIC
    ${vnm_terminal_qt_targets}
)

target_compile_definitions(vnm_terminal PUBLIC
    VNM_TERMINAL_PROFILING_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_PROFILING}>
)

if(MSVC)
    target_compile_options(vnm_terminal PRIVATE
        /FS
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
    find_library(VNM_TERMINAL_UTIL_LIBRARY util)
    if(VNM_TERMINAL_UTIL_LIBRARY)
        target_link_libraries(vnm_terminal PRIVATE
            ${VNM_TERMINAL_UTIL_LIBRARY}
        )
    endif()
endif()

cmake_language(DEFER CALL vnm_terminal_validate_qt_link_interface vnm_terminal)

add_library(vnm_terminal::vnm_terminal ALIAS vnm_terminal)

add_executable(vnm_terminal_canvas_fixture
    tools/terminal_canvas_fixture/terminal_canvas_fixture.cpp
)

set_target_properties(vnm_terminal_canvas_fixture PROPERTIES
    AUTOMOC OFF
)

target_include_directories(vnm_terminal_canvas_fixture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(VNM_TERMINAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

include(CTest)

if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT BUILD_TESTING)
    message(FATAL_ERROR
        "VNM_TERMINAL_BUILD_REQUIRED_READINESS requires BUILD_TESTING=ON")
endif()

if(VNM_TERMINAL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/more_tests/CMakeLists.txt")
        add_subdirectory(more_tests)
    endif()
endif()
