if(NOT VNM_TERMINAL_SOURCE_ROOT OR NOT VNM_TERMINAL_TEST_ROOT)
    message(FATAL_ERROR "Source and test roots are required")
endif()

# The comparison that decides whether a dependency source override was honoured.
# The configure-time half of vnm_terminal_require_dependency_origin reads one
# target property; this is the part with the judgment in it, and a build is a
# poor place to discover that it accepts a tree it should refuse.
include("${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_dependency_origin.cmake")

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
file(MAKE_DIRECTORY "${VNM_TERMINAL_TEST_ROOT}/vnm_qt_dispatch/src")
file(MAKE_DIRECTORY "${VNM_TERMINAL_TEST_ROOT}/vnm_qt_dispatch-src")

set(requested "${VNM_TERMINAL_TEST_ROOT}/vnm_qt_dispatch")

function(expect_honoured label actual)
    vnm_terminal_dependency_origin_violation(problem
        VNM_QT_DISPATCH_SOURCE_DIR "${requested}" "${actual}")
    if(problem)
        message(FATAL_ERROR
            "${label}: the override was honoured, but the check reported: "
            "${problem}")
    endif()
endfunction()

function(expect_ignored label actual)
    vnm_terminal_dependency_origin_violation(problem
        VNM_QT_DISPATCH_SOURCE_DIR "${requested}" "${actual}")
    if(NOT problem)
        message(FATAL_ERROR
            "${label}: the override was ignored and the check accepted it.")
    endif()
    if(NOT problem MATCHES "VNM_QT_DISPATCH_SOURCE_DIR")
        message(FATAL_ERROR
            "${label}: the message does not name the override variable: "
            "${problem}")
    endif()
endfunction()

expect_honoured("the requested tree itself" "${requested}")
expect_honoured("a target defined below the requested tree" "${requested}/src")
expect_honoured("a trailing separator" "${requested}/")

# What a provider that ignored the override produces: FetchContent clones into
# the build tree, beside a directory whose name shares the requested prefix.
expect_ignored("a clone under the build tree" "${requested}-src")
expect_ignored("an unrelated tree" "${VNM_TERMINAL_TEST_ROOT}")
expect_ignored("a target with no source directory" "")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    string(REPLACE "/" "\\" windows_style "${requested}")
    string(TOUPPER "${windows_style}" windows_style)
    expect_honoured("a Windows path spelled differently" "${windows_style}")
endif()

# No override passed is not a violation: the provider is free to resolve the
# dependency itself, which is what an ordinary development build does.
vnm_terminal_dependency_origin_violation(problem
    VNM_QT_DISPATCH_SOURCE_DIR "" "${requested}-src")
if(problem)
    message(FATAL_ERROR
        "an absent override must not be reported as ignored: ${problem}")
endif()

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
message(STATUS "Dependency origin contract passed")
