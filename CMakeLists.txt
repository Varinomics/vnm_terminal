cmake_minimum_required(VERSION 3.21)
project(vnm_terminal LANGUAGES C CXX VERSION 1.7.3)

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

if(VNM_TERMINAL_DISTRIBUTION_BUILD)
    if(CMAKE_CONFIGURATION_TYPES)
        if(NOT CMAKE_CONFIGURATION_TYPES STREQUAL "Release")
            message(FATAL_ERROR "Distribution builds require CMAKE_CONFIGURATION_TYPES=Release")
        endif()
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "Distribution builds require CMAKE_BUILD_TYPE=Release")
    endif()

    foreach(language C CXX)
        foreach(flags CMAKE_${language}_FLAGS CMAKE_${language}_FLAGS_RELEASE)
            if("${${flags}}" MATCHES "(^| )(-g([1-9a-zA-Z][^ ]*)?|/Z[iI7])($| )")
                message(FATAL_ERROR "Distribution builds cannot enable debug information in ${flags}")
            endif()
        endforeach()
    endforeach()
    if(MSVC)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
        add_link_options(/DEBUG:NONE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-g0)
        if(MINGW)
            # MinGW's startup objects include DWARF and COFF symbols even in a
            # Release link. Omit these before embedding helpers or signing.
            add_link_options(LINKER:--strip-all)
        endif()
    endif()
endif()

add_subdirectory(portable_launcher)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickLayouts QuickControls2 QuickShapes)
if(QT_QML_NO_CACHEGEN)
    message(FATAL_ERROR "vnm_terminal requires QML cache generation")
endif()
# The one CPack component this build packages. It is named here rather than in
# cmake/vnm_terminal_packaging.cmake, which is included at the end of this file,
# because dependencies must know it before registering their install rules.
set(vnm_terminal_runtime_component vnm_terminal_runtime)
if(VNM_TERMINAL_BUILD_STANDALONE_APP)
    set(VNM_FONTS_INSTALL_COMPONENT "${vnm_terminal_runtime_component}"
        CACHE STRING "CPack component for the shared font package" FORCE)
endif()

include(cmake/vnm_terminal_dependency_origin.cmake)
include(cmake/vnm_terminal_surface_dependency.cmake)
include(cmake/vnm_qml_chrome_dependency.cmake)
include(cmake/vnm_fonts_dependency.cmake)

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

if(VNM_TERMINAL_BUILD_STANDALONE_APP)
    include(cmake/vnm_environment_policy_dependency.cmake)
    vnm_terminal_environment_policy_make_available()
else()
    set(VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER "not included")
endif()

set(vnm_terminal_build_info_header
    "${CMAKE_CURRENT_BINARY_DIR}/generated/vnm_terminal_build_info.h")
string(TIMESTAMP vnm_terminal_build_date "%Y-%m-%d %H:%M:%S UTC" UTC)
add_custom_target(vnm_terminal_build_info ALL
    COMMAND
        "${CMAKE_COMMAND}"
        "-DVNM_TERMINAL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DVNM_TERMINAL_SURFACE_SOURCE_DIR=${VNM_TERMINAL_SURFACE_SOURCE_DIR}"
        "-DVNM_QML_CHROME_SOURCE_DIR=${VNM_QML_CHROME_SOURCE_DIR}"
        "-DVNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER=${VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER}"
        "-DVNM_TERMINAL_BUILD_DATE=${vnm_terminal_build_date}"
        "-DVNM_TERMINAL_BUILD_INFO_OUTPUT=${vnm_terminal_build_info_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vnm_terminal_generate_build_info.cmake"
    BYPRODUCTS
        "${vnm_terminal_build_info_header}"
    COMMENT "Refreshing build provenance"
    VERBATIM)

set(vnm_terminal_app_support_headers
    include/vnm_terminal/app_support/app_hyperlink_policy.h
    include/vnm_terminal/app_support/app_settings.h
    include/vnm_terminal/app_support/backend_output_capture_json.h
    include/vnm_terminal/app_support/codex_command_environment.h
    include/vnm_terminal/app_support/terminal_display_settings.h
    include/vnm_terminal/app_support/terminal_settings_reconciler.h
    include/vnm_terminal/app_support/diagnostic_sink.h
    include/vnm_terminal/app_support/app_shortcuts.h
    include/vnm_terminal/app_support/qml_chrome.h
    include/vnm_terminal/app_support/terminal_scrollbar.h
    include/vnm_terminal/app_support/terminal_search_bar.h
    include/vnm_terminal/app_support/terminal_settings_controller.h
    include/vnm_terminal/app_support/terminal_settings_dialog.h
    include/vnm_terminal/app_support/terminal_settings_model.h
    include/vnm_terminal/app_support/terminal_settings_window.h
)

qt_add_resources(vnm_terminal_codex_resource_sources
    resources/codex_commands.qrc
)

if(WIN32)
    # Embed the launcher so consumers of app_support need no separately deployed executable.
    add_executable(vnm_terminal_codex_launcher src/windows_codex_launcher.cpp)
    set_target_properties(vnm_terminal_codex_launcher PROPERTIES
        AUTOMOC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/codex_launcher/$<CONFIG>"
    )
    if(MINGW)
        target_link_options(vnm_terminal_codex_launcher PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    endif()
    set(vnm_terminal_codex_launcher_resource
        "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/codex_launcher_resource.cpp")
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/codex_launcher_$<CONFIG>.qrc"
        CONTENT [=[
<RCC>
  <qresource prefix="/vnm_terminal/codex">
    <file alias="codex_launcher.exe">../codex_launcher/$<CONFIG>/vnm_terminal_codex_launcher.exe</file>
  </qresource>
</RCC>
]=])
    add_custom_command(
        OUTPUT "${vnm_terminal_codex_launcher_resource}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
        COMMAND Qt6::rcc --name codex_launcher
            --output "${vnm_terminal_codex_launcher_resource}"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/codex_launcher_$<CONFIG>.qrc"
        DEPENDS vnm_terminal_codex_launcher "$<TARGET_FILE:vnm_terminal_codex_launcher>"
        VERBATIM
    )
    list(APPEND vnm_terminal_codex_resource_sources "${vnm_terminal_codex_launcher_resource}")
endif()

add_library(vnm_terminal_app_support STATIC
    ${vnm_terminal_codex_resource_sources}
    src/app_hyperlink_policy.cpp
    include/vnm_terminal/app_support/app_hyperlink_policy.h
    src/codex_command_environment.cpp
    include/vnm_terminal/app_support/codex_command_environment.h
    src/backend_output_capture_json.cpp
    include/vnm_terminal/app_support/backend_output_capture_json.h
    src/app_diagnostic_sink.cpp
    include/vnm_terminal/app_support/diagnostic_sink.h
    src/app_settings.cpp
    src/app_settings_internal.h
    src/terminal_display_settings.cpp
    include/vnm_terminal/app_support/app_settings.h
    include/vnm_terminal/app_support/terminal_display_settings.h
    src/terminal_settings_reconciler.cpp
    src/terminal_settings_fields.h
    include/vnm_terminal/app_support/terminal_settings_reconciler.h
    src/app_shortcuts.cpp
    include/vnm_terminal/app_support/app_shortcuts.h
    src/qml_chrome.cpp
    src/qml_component_error_string.h
    include/vnm_terminal/app_support/qml_chrome.h
    src/terminal_scrollbar.cpp
    include/vnm_terminal/app_support/terminal_scrollbar.h
    src/terminal_search_bar.cpp
    include/vnm_terminal/app_support/terminal_search_bar.h
    src/terminal_file_drop.cpp
    src/terminal_file_drop.h
    src/terminal_settings_controller.cpp
    include/vnm_terminal/app_support/terminal_settings_controller.h
    src/terminal_settings_dialog.cpp
    include/vnm_terminal/app_support/terminal_settings_dialog.h
    src/terminal_settings_model.cpp
    include/vnm_terminal/app_support/terminal_settings_model.h
    src/terminal_settings_window.cpp
    src/terminal_settings_native_window_owner.h
    include/vnm_terminal/app_support/terminal_settings_window.h
)
add_dependencies(vnm_terminal_app_support vnm_terminal_build_info)
set(vnm_terminal_qml_files
    qml/terminal_qml_chrome.qml
    qml/terminal_search_bar.qml
    qml/terminal_settings_window.qml
)
foreach(qml_file IN LISTS vnm_terminal_qml_files)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
vnm_add_qml_resources(vnm_terminal_app_support
    URI vnm_terminal
    VERSION 1.0
    RESOURCE_PREFIX "/"
    GENERATE_QMLDIR
    QML_FILES ${vnm_terminal_qml_files}
)
get_target_property(vnm_terminal_qml_output_targets
    vnm_terminal_app_support VNM_QML_RESOURCE_TARGETS)
target_include_directories(vnm_terminal_app_support
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
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
    PRIVATE
        vnm_qml_chrome::vnm_qml_chrome
        vnm::fonts
)
add_library(
    vnm_terminal::vnm_terminal_app_support
    ALIAS vnm_terminal_app_support)
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
    get_target_property(vnm_terminal_chrome_qml_output_targets
        vnm_qml_chrome VNM_QML_RESOURCE_TARGETS)

    if(COMMAND vnm_fonts_install_package)
        vnm_fonts_install_package()
    endif()
    install(TARGETS
        vnm_terminal_app_support
        vnm_terminal_default_shell
        vnm_qml_chrome
        ${vnm_terminal_qml_output_targets}
        ${vnm_terminal_chrome_qml_output_targets}
        EXPORT vnm_terminalTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}"
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

find_package(Qt6 6.12 REQUIRED COMPONENTS Svg)
find_package(Qt6Svg 6.12 REQUIRED COMPONENTS
    QSvgPlugin
)

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
    src/app_metrics.cpp
    src/app_metrics.h
    src/app_options.cpp
    src/app_options.h
    src/app_options_settings_adapter.cpp
    src/app_options_settings_adapter.h
    src/app_settings_keys.h
    src/standalone_environment.cpp
    src/standalone_environment.h
    src/standalone_native_receipts.cpp
    src/standalone_native_receipts.h
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
    vnm_terminal::vnm_terminal_environment_policy
    Qt6::QuickControls2
    Qt6::Svg
)
if(APPLE)
    target_link_libraries(vnm_terminal PRIVATE vnm_process_custody::vnm_process_custody)
endif()

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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    vnm_process_custody_deploy_owner(vnm_terminal)
endif()

if(WIN32)
    vnm_terminal_deploy_conpty(vnm_terminal)

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

    find_package(Qt6Gui 6.12 REQUIRED COMPONENTS
        QICOPlugin
        QWindowsIntegrationPlugin
    )
    find_package(Qt6 6.12 REQUIRED COMPONENTS
        QuickControls2Basic
        QuickControls2BasicStyleImpl
        QuickControls2Impl
    )

    vnm_terminal_copy_qt_plugin(Qt6::QWindowsIntegrationPlugin platforms)
    vnm_terminal_copy_qt_plugin(Qt6::QICOPlugin imageformats)
    vnm_terminal_copy_qt_plugin(Qt6::QSvgPlugin imageformats)
    vnm_terminal_copy_qt_runtime_library(Qt6::Svg)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickShapes)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2Basic)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2BasicStyleImpl)
    vnm_terminal_copy_qt_runtime_library(Qt6::QuickControls2Impl)

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
        vnm_terminal::portable_launcher_process
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
