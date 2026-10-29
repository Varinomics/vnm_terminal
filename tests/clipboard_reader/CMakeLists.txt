find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
find_package(Qt6Gui 6.12 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)

add_executable(vnm_terminal_app_clipboard_reader
    ../terminal_app/app_clipboard_reader_tests.cpp)
target_include_directories(vnm_terminal_app_clipboard_reader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(vnm_terminal_app_clipboard_reader PRIVATE vnm_terminal::vnm_terminal_app_support)
set_target_properties(vnm_terminal_app_clipboard_reader PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(vnm_terminal_app_clipboard_reader PRIVATE /FS)
endif()

if(WIN32)
    set(clipboard_qt_runtime "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(clipboard_qt_runtime "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(clipboard_qt_runtime "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()
add_test(NAME vnm_terminal_app_clipboard_reader
    COMMAND "${CMAKE_COMMAND}" -E env "${clipboard_qt_runtime}"
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        "$<TARGET_FILE:vnm_terminal_app_clipboard_reader>")
set_tests_properties(vnm_terminal_app_clipboard_reader PROPERTIES TIMEOUT 30)

if(WIN32)
    find_package(Qt6Gui 6.12 REQUIRED COMPONENTS QWindowsIntegrationPlugin)
    add_executable(vnm_terminal_hosted_clipboard hosted_clipboard_tests.cpp)
    target_link_libraries(vnm_terminal_hosted_clipboard PRIVATE
        vnm_terminal::vnm_terminal_app_support Qt6::Test)
    add_dependencies(vnm_terminal_hosted_clipboard vnm_terminal_clipboard_broker)
    add_custom_command(TARGET vnm_terminal_hosted_clipboard POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:vnm_terminal_clipboard_broker>"
            "$<TARGET_FILE_DIR:vnm_terminal_hosted_clipboard>/vnm_terminal_clipboard_broker.exe"
        VERBATIM)
    add_test(NAME vnm_terminal_hosted_clipboard
        COMMAND "${CMAKE_COMMAND}" -E env "${clipboard_qt_runtime}"
            "QT_QPA_PLATFORM=windows"
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE:vnm_terminal_hosted_clipboard>" -o -,txt)
    set_tests_properties(vnm_terminal_hosted_clipboard PROPERTIES
        LABELS "windows;clipboard;hosted" TIMEOUT 60 RUN_SERIAL TRUE)
endif()
