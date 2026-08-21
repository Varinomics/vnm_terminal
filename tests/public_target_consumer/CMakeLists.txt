cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_public_target_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vnm_terminal CONFIG REQUIRED)

set(vnm_terminal_public_headers
    vnm_terminal/app_support/app_settings.h
    vnm_terminal/app_support/app_shortcuts.h
    vnm_terminal/app_support/qml_chrome.h
    vnm_terminal/app_support/terminal_scrollbar.h
    vnm_terminal/app_support/terminal_search_bar.h
    vnm_terminal/app_support/terminal_settings_controller.h
    vnm_terminal/app_support/terminal_settings_window.h
    vnm_terminal/default_shell.h
)
set(vnm_terminal_header_index 0)
foreach(vnm_terminal_public_header IN LISTS vnm_terminal_public_headers)
    set(vnm_terminal_header_source
        "${CMAKE_CURRENT_BINARY_DIR}/h${vnm_terminal_header_index}.cpp")
    file(GENERATE
        OUTPUT "${vnm_terminal_header_source}"
        CONTENT "#include <${vnm_terminal_public_header}>\n")
    add_library(
        "vnm_terminal_header_${vnm_terminal_header_index}"
        OBJECT
        "${vnm_terminal_header_source}")
    target_link_libraries(
        "vnm_terminal_header_${vnm_terminal_header_index}"
        PRIVATE
        vnm_terminal::vnm_terminal_app_support
        vnm_terminal::vnm_terminal_default_shell)
    math(EXPR vnm_terminal_header_index "${vnm_terminal_header_index} + 1")
endforeach()

add_executable(vnm_terminal_public_target_consumer main.cpp)
target_link_libraries(vnm_terminal_public_target_consumer PRIVATE
    vnm_terminal::vnm_terminal_app_support
    vnm_terminal::vnm_terminal_default_shell
)
