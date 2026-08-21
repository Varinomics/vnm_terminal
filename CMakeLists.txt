cmake_minimum_required(VERSION 3.21)
project(vnm_terminal LANGUAGES C CXX VERSION 1.4.6)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
option(VNM_TERMINAL_BUILD_PACKAGES
    "Generate self-contained platform packages from the install tree"
    OFF)
option(VNM_TERMINAL_BUILD_STANDALONE_APP
    "Build the standalone vnm_terminal application"
    ON)

if(VNM_TERMINAL_DISTRIBUTION_BUILD AND VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    message(FATAL_ERROR
        "VNM_TERMINAL_DISTRIBUTION_BUILD=ON requires "
        "VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=OFF")
endif()

if(VNM_TERMINAL_BUILD_PACKAGES AND NOT VNM_TERMINAL_DISTRIBUTION_BUILD)
    message(FATAL_ERROR
        "VNM_TERMINAL_BUILD_PACKAGES=ON requires "
        "VNM_TERMINAL_DISTRIBUTION_BUILD=ON")
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickLayouts QuickControls2)
include(cmake/vnm_terminal_dependency_origin.cmake)
include(cmake/vnm_terminal_surface_dependency.cmake)
include(cmake/vnm_qml_chrome_dependency.cmake)

if(NOT TARGET vnm_qt_dispatch::vnm_qt_dispatch)
    message(FATAL_ERROR
        "vnm_terminal_surface did not make "
        "vnm_qt_dispatch::vnm_qt_dispatch available.")
endif()

# These two cache variables are declared by vnm_terminal_surface, so this build
# passes them without owning them. If the surface stops reading one, CMake
# reports an unused variable and configures successfully while the surface
# fetches that dependency from master instead, which is the one outcome the
# dependency lock and the provenance record cannot detect.
vnm_terminal_require_dependency_origin(
    vnm_qt_dispatch::vnm_qt_dispatch VNM_QT_DISPATCH_SOURCE_DIR)
vnm_terminal_require_dependency_origin(
    vnm_msdf_text::vnm_msdf_text VNM_TERMINAL_MSDF_TEXT_RENDERER_SOURCE_DIR)

set(vnm_terminal_build_info_header
    "${CMAKE_CURRENT_BINARY_DIR}/generated/vnm_terminal_build_info.h")
string(TIMESTAMP vnm_terminal_build_date "%Y-%m-%d %H:%M:%S UTC" UTC)
add_custom_target(vnm_terminal_build_info ALL
    COMMAND
        "${CMAKE_COMMAND}"
        "-DVNM_TERMINAL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DVNM_TERMINAL_SURFACE_SOURCE_DIR=${VNM_TERMINAL_SURFACE_SOURCE_DIR}"
        "-DVNM_QML_CHROME_SOURCE_DIR=${VNM_QML_CHROME_SOURCE_DIR}"
        "-DVNM_TERMINAL_BUILD_DATE=${vnm_terminal_build_date}"
        "-DVNM_TERMINAL_BUILD_INFO_OUTPUT=${vnm_terminal_build_info_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vnm_terminal_generate_build_info.cmake"
    BYPRODUCTS
        "${vnm_terminal_build_info_header}"
    COMMENT "Refreshing build provenance"
    VERBATIM)

set(vnm_terminal_app_support_headers
    include/vnm_terminal/app_support/app_settings.h
    include/vnm_terminal/app_support/app_shortcuts.h
    include/vnm_terminal/app_support/qml_chrome.h
    include/vnm_terminal/app_support/terminal_scrollbar.h
    include/vnm_terminal/app_support/terminal_search_bar.h
    include/vnm_terminal/app_support/terminal_settings_controller.h
    include/vnm_terminal/app_support/terminal_settings_window.h
)

add_library(vnm_terminal_app_support STATIC
    src/app_settings.cpp
    include/vnm_terminal/app_support/app_settings.h
    src/app_shortcuts.cpp
    include/vnm_terminal/app_support/app_shortcuts.h
    src/qml_chrome.cpp
    include/vnm_terminal/app_support/qml_chrome.h
    src/terminal_scrollbar.cpp
    include/vnm_terminal/app_support/terminal_scrollbar.h
    src/terminal_search_bar.cpp
    include/vnm_terminal/app_support/terminal_search_bar.h
    src/terminal_settings_controller.cpp
    include/vnm_terminal/app_support/terminal_settings_controller.h
    src/terminal_settings_window.cpp
    include/vnm_terminal/app_support/terminal_settings_window.h
)
add_dependencies(vnm_terminal_app_support vnm_terminal_build_info)
get_target_property(
    vnm_terminal_qml_chrome_include_dirs
    vnm_qml_chrome::vnm_qml_chrome
    INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(vnm_terminal_app_support
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        ${vnm_terminal_qml_chrome_include_dirs}
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_definitions(vnm_terminal_app_support PRIVATE
    VNM_TERMINAL_BUILD_PROVENANCE_HEADER="vnm_terminal_build_info.h"
)
target_link_libraries(vnm_terminal_app_support
    PUBLIC
        vnm_terminal_surface::vnm_terminal_surface
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
)
# A source-tree consumer needs the source chrome target, while the exported
# archive's package config appends the co-installed chrome runtime target. Keep
# the local target out of the export set with this build-only wrapper.
add_library(vnm_terminal_app_support_source INTERFACE)
target_link_libraries(vnm_terminal_app_support_source INTERFACE
    vnm_terminal_app_support
    vnm_qml_chrome::vnm_qml_chrome)
add_library(
    vnm_terminal::vnm_terminal_app_support
    ALIAS vnm_terminal_app_support_source)

add_library(vnm_terminal_default_shell STATIC
    include/vnm_terminal/default_shell.h
    src/default_shell.cpp
)
add_library(
    vnm_terminal::vnm_terminal_default_shell
    ALIAS vnm_terminal_default_shell)
target_include_directories(vnm_terminal_default_shell PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(vnm_terminal_default_shell PUBLIC Qt6::Core)

if(MSVC)
    target_compile_options(vnm_terminal_app_support PRIVATE /FS)
    target_compile_options(vnm_terminal_default_shell PRIVATE /FS)
endif()

if(PROJECT_IS_TOP_LEVEL)
    get_target_property(
        vnm_terminal_qml_chrome_is_imported
        vnm_qml_chrome::vnm_qml_chrome
        IMPORTED)
    if(vnm_terminal_qml_chrome_is_imported)
        message(FATAL_ERROR
            "Installing vnm_terminal app support requires a source-built "
            "vnm_qml_chrome target because vnm_qml_chrome does not publish "
            "an installed CMake package.")
    endif()
    get_target_property(
        vnm_terminal_qml_chrome_source_dir
        vnm_qml_chrome::vnm_qml_chrome
        SOURCE_DIR)
    set_target_properties(vnm_qml_chrome PROPERTIES
        EXPORT_NAME vnm_terminal_qml_chrome_runtime)
    install(TARGETS
        vnm_terminal_app_support
        vnm_terminal_default_shell
        vnm_qml_chrome
        EXPORT vnm_terminalTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(FILES ${vnm_terminal_app_support_headers}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vnm_terminal/app_support")
    install(FILES include/vnm_terminal/default_shell.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vnm_terminal")
    install(DIRECTORY
        "${vnm_terminal_qml_chrome_source_dir}/include/vnm_qml_chrome"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT vnm_terminalTargets
        NAMESPACE vnm_terminal::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal")

    configure_package_config_file(
        cmake/vnm_terminalConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminalConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminalConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminalConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminalConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal")
endif()

if(NOT VNM_TERMINAL_BUILD_STANDALONE_APP)
    return()
endif()

include(cmake/vnm_environment_policy_dependency.cmake)
vnm_terminal_environment_policy_make_available()

set(vnm_terminal_sources
    src/main.cpp
    src/app_cli.cpp
    src/app_cli.h
    src/app_clipboard_reader.cpp
    src/app_clipboard_reader.h
    src/app_clipboard_policy.cpp
    src/app_clipboard_policy.h
    src/app_common.cpp
    src/app_common.h
    src/app_hyperlink_policy.cpp
    src/app_hyperlink_policy.h
    src/app_metrics.cpp
    src/app_metrics.h
    src/app_options.cpp
    src/app_options.h
    src/app_options_settings_adapter.cpp
    src/app_options_settings_adapter.h
    src/app_settings_keys.h
    src/standalone_environment.cpp
    src/standalone_environment.h
    src/app_presentation_metrics.cpp
    src/app_presentation_metrics.h
    src/app_profile_text.cpp
    src/app_profile_text.h
    src/terminal_title_metadata.h
    src/terminal_window.cpp
    src/terminal_window.h
)

if(VNM_TERMINAL_BUILD_PACKAGES)
    qt_add_executable(vnm_terminal
        ${vnm_terminal_sources}
        packaging/vnm_terminal_runtime_imports.qml
    )
else()
    add_executable(vnm_terminal
        ${vnm_terminal_sources}
    )
endif()

add_dependencies(vnm_terminal vnm_terminal_build_info)

target_include_directories(vnm_terminal PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
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
    vnm_terminal::vnm_terminal_app_support
    vnm_terminal::vnm_terminal_default_shell
    vnm_terminal_surface::vnm_terminal_surface
    vnm_qt_dispatch::vnm_qt_dispatch
    vnm_qml_chrome::vnm_qml_chrome
    vnm_framework::vnm_environment_policy
)

target_compile_definitions(vnm_terminal PRIVATE
    VNM_TERMINAL_VERSION_STRING="${PROJECT_VERSION}"
    VNM_TERMINAL_BUILD_PROVENANCE_HEADER="vnm_terminal_build_info.h"
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

    find_package(Qt6Gui 6.11 REQUIRED COMPONENTS
        QICOPlugin
        QWindowsIntegrationPlugin
    )
    find_package(Qt6Svg 6.11 REQUIRED COMPONENTS
        QSvgPlugin
    )
    find_package(Qt6 6.11 REQUIRED COMPONENTS
        QuickControls2Basic
        QuickControls2BasicStyleImpl
        QuickControls2Fusion
        QuickControls2FusionStyleImpl
        QuickControls2Impl
        QuickControls2WindowsStyleImpl
    )

    vnm_terminal_copy_qt_plugin(Qt6::QWindowsIntegrationPlugin platforms)
    vnm_terminal_copy_qt_plugin(Qt6::QICOPlugin imageformats)
    vnm_terminal_copy_qt_plugin(Qt6::QSvgPlugin imageformats)
    vnm_terminal_copy_qt_runtime_library(Qt6::Svg)
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
        src/portable_launcher_text.c
        src/portable_launcher_text.h
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

include(cmake/vnm_terminal_packaging.cmake)
