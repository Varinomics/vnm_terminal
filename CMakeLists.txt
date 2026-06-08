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

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Qml Quick QuickLayouts QuickControls2)
include(cmake/vnm_terminal_surface_dependency.cmake)
include(cmake/vnm_qml_chrome_dependency.cmake)

add_executable(vnm_terminal
    src/main.cpp
    src/app_clipboard_policy.cpp
    src/app_clipboard_policy.h
    src/app_common.cpp
    src/app_common.h
    src/app_options.cpp
    src/app_options.h
    src/app_settings.cpp
    src/app_settings.h
    src/app_shortcuts.cpp
    src/app_shortcuts.h
    src/qml_chrome.cpp
    src/qml_chrome.h
    src/terminal_scrollbar.cpp
    src/terminal_scrollbar.h
    src/terminal_title_metadata.h
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
    vnm_qml_chrome::vnm_qml_chrome
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

    function(vnm_terminal_copy_qt_runtime_library library_target)
        if(NOT TARGET ${library_target})
            message(FATAL_ERROR
                "Required Qt runtime target ${library_target} is missing")
        endif()

        get_target_property(library_target_type ${library_target} TYPE)
        if(NOT library_target_type STREQUAL "SHARED_LIBRARY")
            message(STATUS
                "Skipping runtime copy for ${library_target}; target type is ${library_target_type}")
            return()
        endif()

        add_custom_command(TARGET vnm_terminal POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${library_target}>"
                "$<TARGET_FILE_DIR:vnm_terminal>/$<TARGET_FILE_NAME:${library_target}>"
        )
    endfunction()

    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS
        QICOPlugin
        QWindowsIntegrationPlugin
    )
    find_package(Qt6 6.7 REQUIRED COMPONENTS
        QuickControls2Basic
        QuickControls2BasicStyleImpl
        QuickControls2Fusion
        QuickControls2FusionStyleImpl
        QuickControls2Impl
        QuickControls2WindowsStyleImpl
    )

    vnm_terminal_copy_qt_plugin(Qt6::QWindowsIntegrationPlugin platforms)
    vnm_terminal_copy_qt_plugin(Qt6::QICOPlugin imageformats)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2Basic)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2BasicStyleImpl)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2Fusion)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2FusionStyleImpl)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2Impl)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2WindowsStyleImpl)

    add_custom_command(TARGET vnm_terminal POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_RUNTIME_DLLS:vnm_terminal>
            "$<TARGET_FILE_DIR:vnm_terminal>"
        COMMAND_EXPAND_LISTS
    )

    function(vnm_terminal_copy_qt_qml_module module_subdir)
        add_custom_command(TARGET vnm_terminal POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:vnm_terminal>/qml/${module_subdir}"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_FILE_DIR:Qt6::Core>/../qml/${module_subdir}"
                "$<TARGET_FILE_DIR:vnm_terminal>/qml/${module_subdir}"
        )
    endfunction()

    vnm_terminal_copy_qt_qml_module(QtQml)
    vnm_terminal_copy_qt_qml_module(QtQuick)

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
