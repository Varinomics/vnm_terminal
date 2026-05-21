add_executable(vnm_terminal_example_terminal
    frameless_resize_filter.cpp
    frameless_resize_filter.h
    main.cpp
    terminal_scrollbar.cpp
    terminal_scrollbar.h
    window_chrome.cpp
    window_chrome.h
    window_chrome_model.h
)

qt_add_resources(vnm_terminal_example_terminal "vnm_terminal_example_terminal_resources"
    PREFIX
        "/vnm_terminal_example_terminal"
    BASE
        "."
    FILES
        vnm_terminal.ico
)

target_link_libraries(vnm_terminal_example_terminal PRIVATE
    vnm_terminal::vnm_terminal
)

if(MSVC)
    target_compile_options(vnm_terminal_example_terminal PRIVATE
        /FS
    )
endif()

if(WIN32)
    set_target_properties(vnm_terminal_example_terminal PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    target_sources(vnm_terminal_example_terminal PRIVATE
        vnm_terminal_example_terminal.rc
    )

    function(vnm_terminal_example_copy_qt_platform_plugin plugin_target)
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

        add_custom_command(TARGET vnm_terminal_example_terminal POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:vnm_terminal_example_terminal>/platforms"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${plugin_target}>"
                "$<TARGET_FILE_DIR:vnm_terminal_example_terminal>/platforms/$<TARGET_FILE_NAME:${plugin_target}>"
        )
    endfunction()

    find_package(Qt6Gui 6.7 REQUIRED COMPONENTS
        QWindowsIntegrationPlugin
    )

    vnm_terminal_example_copy_qt_platform_plugin(Qt6::QWindowsIntegrationPlugin)
endif()

cmake_language(DEFER CALL
    vnm_terminal_validate_qt_link_interface
    vnm_terminal_example_terminal)
