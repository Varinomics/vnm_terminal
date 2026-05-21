cmake_minimum_required(VERSION 3.21)
project(vnm_terminal LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(VNM_TERMINAL_ENABLE_PROFILING
    "Compile terminal surface profile scope instrumentation"
    OFF)

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

target_link_libraries(vnm_terminal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

target_compile_definitions(vnm_terminal PRIVATE
    VNM_TERMINAL_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(vnm_terminal PRIVATE
        /FS
    )
endif()

if(WIN32)
    set_target_properties(vnm_terminal PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    target_sources(vnm_terminal PRIVATE
        src/vnm_terminal.rc
    )

    function(vnm_terminal_copy_qt_platform_plugin plugin_target)
        if(NOT TARGET ${plugin_target})
            message(FATAL_ERROR
                "Required Qt platform plugin target ${plugin_target} is missing")
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
                "$<TARGET_FILE_DIR:vnm_terminal>/platforms"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${plugin_target}>"
                "$<TARGET_FILE_DIR:vnm_terminal>/platforms/$<TARGET_FILE_NAME:${plugin_target}>"
        )
    endfunction()

    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS
        QWindowsIntegrationPlugin
    )

    vnm_terminal_copy_qt_platform_plugin(Qt6::QWindowsIntegrationPlugin)

    add_executable(vnm_terminal_portable_launcher WIN32
        src/portable_launcher_win.c
    )
    target_link_libraries(vnm_terminal_portable_launcher PRIVATE
        shell32
    )
endif()

include(CTest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
