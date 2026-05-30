cmake_minimum_required(VERSION 3.21)
project(vnm_terminal LANGUAGES C CXX VERSION 1.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(VNM_TERMINAL_ENABLE_PROFILING
    "Compile terminal surface profile scope instrumentation"
    OFF)
option(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY
    "Build diagnostic transcript capture/replay support in the app and surface dependency"
    OFF)
option(VNM_TERMINAL_DISTRIBUTION_BUILD
    "Configure a distribution build; diagnostic transcript capture/replay must stay disabled"
    OFF)
option(VNM_TERMINAL_APP_BUILD_BENCHMARKS
    "Build opt-in vnm_terminal benchmark runners"
    OFF)

if(VNM_TERMINAL_DISTRIBUTION_BUILD AND VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    message(FATAL_ERROR
        "VNM_TERMINAL_DISTRIBUTION_BUILD=ON requires "
        "VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=OFF")
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Quick)
include(cmake/vnm_terminal_surface_dependency.cmake)

add_executable(vnm_terminal
    src/frameless_resize_filter.cpp
    src/frameless_resize_filter.h
    src/main.cpp
    src/terminal_scrollbar.cpp
    src/terminal_scrollbar.h
    src/window_chrome.cpp
    src/window_chrome.h
    src/window_chrome_model.h
)

qt_add_resources(vnm_terminal "vnm_terminal_resources"
    PREFIX
        "/vnm_terminal"
    BASE
        "src"
    FILES
        src/vnm_terminal.ico
)

set(vnm_terminal_macos_icon "${CMAKE_CURRENT_SOURCE_DIR}/src/vnm_terminal.icns")
if(APPLE AND EXISTS "${vnm_terminal_macos_icon}")
    target_sources(vnm_terminal PRIVATE
        "${vnm_terminal_macos_icon}"
    )
    set_source_files_properties("${vnm_terminal_macos_icon}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
endif()

target_link_libraries(vnm_terminal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

target_compile_definitions(vnm_terminal PRIVATE
    VNM_TERMINAL_VERSION_STRING="${PROJECT_VERSION}"
    VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}>
)

if(MSVC)
    target_compile_options(vnm_terminal PRIVATE
        /FS
    )
endif()

if(APPLE)
    set_target_properties(vnm_terminal PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "vnm_terminal"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright Varinomics"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.varinomics.vnm-terminal"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )

    if(EXISTS "${vnm_terminal_macos_icon}")
        set_target_properties(vnm_terminal PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "vnm_terminal.icns"
        )
    endif()
endif()

if(WIN32)
    set_target_properties(vnm_terminal PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    target_sources(vnm_terminal PRIVATE
        src/vnm_terminal.rc
    )

    function(vnm_terminal_copy_qt_plugin plugin_target plugin_subdir)
        if(NOT TARGET ${plugin_target})
            message(FATAL_ERROR
                "Required Qt plugin target ${plugin_target} is missing")
        endif()

        get_target_property(plugin_target_type ${plugin_target} TYPE)
        if(NOT plugin_target_type MATCHES "^(MODULE_LIBRARY|SHARED_LIBRARY)$")
            message(STATUS
                "Skipping runtime copy for ${plugin_target}; target type is ${plugin_target_type}")
            return()
        endif()

        add_custom_command(TARGET vnm_terminal POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:vnm_terminal>/${plugin_subdir}"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${plugin_target}>"
                "$<TARGET_FILE_DIR:vnm_terminal>/${plugin_subdir}/$<TARGET_FILE_NAME:${plugin_target}>"
        )
    endfunction()

    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS
        QICOPlugin
        QWindowsIntegrationPlugin
    )

    vnm_terminal_copy_qt_plugin(Qt6::QWindowsIntegrationPlugin platforms)
    vnm_terminal_copy_qt_plugin(Qt6::QICOPlugin imageformats)

    add_custom_command(TARGET vnm_terminal POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_RUNTIME_DLLS:vnm_terminal>
            "$<TARGET_FILE_DIR:vnm_terminal>"
        COMMAND_EXPAND_LISTS
    )

    add_executable(vnm_terminal_portable_launcher WIN32
        src/portable_launcher_win.c
        src/vnm_terminal.rc
    )
    target_link_libraries(vnm_terminal_portable_launcher PRIVATE
        shell32
    )
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_options(vnm_terminal_portable_launcher PRIVATE
            -static
            -static-libgcc
        )
    endif()
endif()

include(CTest)

if(VNM_TERMINAL_APP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
