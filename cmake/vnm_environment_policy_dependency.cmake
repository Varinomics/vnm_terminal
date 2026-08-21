include(FetchContent)

set(VNM_TERMINAL_FRAMEWORK_SOURCE_DIR "" CACHE PATH
    "Optional path to a local vnm_framework checkout")

function(vnm_terminal_environment_policy_make_available)
    if(TARGET vnm_framework::vnm_environment_policy)
        return()
    endif()

    if(VNM_TERMINAL_FRAMEWORK_SOURCE_DIR)
        if(NOT EXISTS "${VNM_TERMINAL_FRAMEWORK_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "VNM_TERMINAL_FRAMEWORK_SOURCE_DIR does not contain a "
                "vnm_framework CMakeLists.txt: "
                "${VNM_TERMINAL_FRAMEWORK_SOURCE_DIR}")
        endif()
        file(REAL_PATH
            "${VNM_TERMINAL_FRAMEWORK_SOURCE_DIR}"
            vnm_terminal_framework_source_dir)
        FetchContent_Declare(vnm_terminal_framework
            SOURCE_DIR "${vnm_terminal_framework_source_dir}"
            BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/vnm_framework-build")
    else()
        FetchContent_Declare(vnm_terminal_framework
            GIT_REPOSITORY https://github.com/imakris/vnm_framework.git
            GIT_TAG master
            GIT_SHALLOW FALSE
            BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/vnm_framework-build")
    endif()

    set(VNM_FRAMEWORK_BUILD_AGGREGATE OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_TEXT OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_MANAGER OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_QML_SHELL OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_SURFACE OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_CONTROL OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_WORKER_RUNTIME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_APP_INIT OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_QML_RUNTIME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_REMOTE_UI_RUNTIME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_REMOTE_UI_RUNTIME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_REMOTE_CONTROL OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_ENABLE_QML_CHROME OFF CACHE BOOL "" FORCE)
    set(VNM_FRAMEWORK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(vnm_terminal_framework)

    if(NOT TARGET vnm_framework::vnm_environment_policy)
        message(FATAL_ERROR
            "vnm_framework did not publish vnm_framework::vnm_environment_policy")
    endif()
endfunction()
