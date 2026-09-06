cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vnm_portable_launcher LANGUAGES C CXX)
    enable_testing()
endif()

add_library(vnm_portable_launcher_text STATIC ../src/portable_launcher_text.c)
add_library(vnm_terminal::portable_launcher_text ALIAS vnm_portable_launcher_text)
target_include_directories(vnm_portable_launcher_text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set_target_properties(vnm_portable_launcher_text PROPERTIES AUTOMOC OFF)

# Source-consumable support also builds independently of the terminal and Qt.
if(WIN32)
    add_library(vnm_portable_launcher_process STATIC ../src/portable_launcher_process.c)
    add_library(vnm_terminal::portable_launcher_process ALIAS vnm_portable_launcher_process)
    target_link_libraries(vnm_portable_launcher_process PUBLIC vnm_portable_launcher_text shell32 user32)
    set_target_properties(vnm_portable_launcher_process PROPERTIES AUTOMOC OFF)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(vnm_portable_launcher_atomic_text ../tests/terminal_app/portable_launcher_atomic_text_tests.c)
    target_link_libraries(vnm_portable_launcher_atomic_text PRIVATE vnm_portable_launcher_text)
    add_test(NAME vnm_portable_launcher_atomic_text COMMAND vnm_portable_launcher_atomic_text)

    add_executable(vnm_terminal_portable_launcher_text ../tests/terminal_app/portable_launcher_text_tests.cpp)
    target_compile_features(vnm_terminal_portable_launcher_text PRIVATE cxx_std_17)
    target_include_directories(vnm_terminal_portable_launcher_text PRIVATE ../tests)
    target_link_libraries(vnm_terminal_portable_launcher_text PRIVATE vnm_portable_launcher_text)
    if(WIN32)
        target_link_libraries(vnm_terminal_portable_launcher_text PRIVATE shell32)
    endif()
    add_test(NAME vnm_terminal_portable_launcher_text COMMAND vnm_terminal_portable_launcher_text)

    if(WIN32)
        add_executable(vnm_terminal_portable_launcher WIN32 ../src/portable_launcher_win.c)
        target_link_libraries(vnm_terminal_portable_launcher PRIVATE vnm_portable_launcher_process)
        add_executable(vnm_terminal_portable_launcher_win ../tests/terminal_app/portable_launcher_win_tests.c)
        target_link_libraries(vnm_terminal_portable_launcher_win PRIVATE vnm_portable_launcher_text shell32 user32)
        add_test(NAME vnm_terminal_portable_launcher_win COMMAND vnm_terminal_portable_launcher_win
            $<TARGET_FILE:vnm_terminal_portable_launcher>)
        set_tests_properties(vnm_terminal_portable_launcher_win PROPERTIES TIMEOUT 60)
    endif()
endif()
