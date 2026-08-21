include_guard(GLOBAL)

# A dependency source override is a promise that the build compiles the checkout
# the caller points at. The cache variables that carry those overrides into
# vnm_terminal_surface are declared by the surface, not here, so a rename or a
# removal there turns the -D this repository passes into an unused cache entry:
# CMake prints "Manually-specified variables were not used by the project",
# configuration succeeds, and the surface resolves the dependency itself from
# the provider's master branch.
#
# Nothing downstream can see that. The release provenance record reads the
# sibling checkout that CI resolved, the signing job compares that record with
# release/dependencies.lock.json, and both agree about source the release does
# not contain. So the override is verified where it lands, at every configure
# site and on every host: the target that was built has to come from the tree
# that was requested.

# The comparison alone, so that a test can drive it without a configured
# dependency. An empty request means no override was passed and there is nothing
# to honour. Everything else must resolve inside the requested tree: a target is
# defined by the directory of its own CMakeLists.txt, which is the dependency
# root or a directory below it.
function(vnm_terminal_dependency_origin_violation
    out_var variable_name requested actual)

    set(${out_var} "" PARENT_SCOPE)
    if("${requested}" STREQUAL "")
        return()
    endif()

    if("${actual}" STREQUAL "")
        string(CONCAT message
            "${variable_name} names ${requested}, but the target it should "
            "have supplied reports no source directory, so this build cannot "
            "say which tree it compiled.")
        set(${out_var} "${message}" PARENT_SCOPE)
        return()
    endif()

    file(TO_CMAKE_PATH "${requested}" requested_path)
    file(TO_CMAKE_PATH "${actual}" actual_path)
    file(REAL_PATH "${requested_path}" requested_path)
    file(REAL_PATH "${actual_path}" actual_path)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        string(TOLOWER "${requested_path}" requested_path)
        string(TOLOWER "${actual_path}" actual_path)
    endif()

    # The separators keep a sibling that merely shares a prefix from passing.
    string(FIND "${actual_path}/" "${requested_path}/" position)
    if(position EQUAL 0)
        return()
    endif()

    string(CONCAT message
        "${variable_name} names ${requested}, but the target it should have "
        "supplied was built from ${actual}. The provider ignored the override "
        "and resolved that dependency itself, so this build contains source "
        "that neither release/dependencies.lock.json nor the provenance record "
        "describes. Check that the provider still declares ${variable_name}.")
    set(${out_var} "${message}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_require_dependency_origin target variable_name)
    if(NOT TARGET ${target})
        return()
    endif()

    # An imported target comes from an installed package rather than from a
    # source tree, and the override is consulted only when no package was found,
    # so there is nothing here to have been ignored.
    get_target_property(vnm_terminal_origin_imported ${target} IMPORTED)
    if(vnm_terminal_origin_imported)
        return()
    endif()

    get_target_property(vnm_terminal_origin_actual ${target} SOURCE_DIR)
    vnm_terminal_dependency_origin_violation(vnm_terminal_origin_problem
        "${variable_name}"
        "${${variable_name}}"
        "${vnm_terminal_origin_actual}")
    if(vnm_terminal_origin_problem)
        message(FATAL_ERROR "${vnm_terminal_origin_problem}")
    endif()
endfunction()

# A source target supplied by an embedding parent is already the selected
# dependency. Record the tree that actually defined it so build provenance does
# not become "unknown" merely because the dependency script did not need to add
# or find the target itself. Imported targets intentionally stay pathless: an
# installed package has no truthful source checkout for this build to report.
function(vnm_terminal_adopt_existing_target_source target variable_name)
    if(NOT TARGET ${target})
        return()
    endif()

    get_target_property(vnm_terminal_origin_imported ${target} IMPORTED)
    if(vnm_terminal_origin_imported)
        get_property(
            vnm_terminal_origin_help
            CACHE ${variable_name}
            PROPERTY HELPSTRING)
        set(${variable_name}
            ""
            CACHE PATH
            "${vnm_terminal_origin_help}"
            FORCE)
        set(${variable_name} "" PARENT_SCOPE)
        return()
    endif()

    get_target_property(vnm_terminal_origin_actual ${target} SOURCE_DIR)
    if(NOT vnm_terminal_origin_actual)
        message(FATAL_ERROR
            "${target} is a source-built target but reports no SOURCE_DIR, "
            "so vnm_terminal cannot record its build provenance.")
    endif()

    vnm_terminal_dependency_origin_violation(vnm_terminal_origin_problem
        "${variable_name}"
        "${${variable_name}}"
        "${vnm_terminal_origin_actual}")
    if(vnm_terminal_origin_problem)
        message(FATAL_ERROR "${vnm_terminal_origin_problem}")
    endif()

    get_property(
        vnm_terminal_origin_help
        CACHE ${variable_name}
        PROPERTY HELPSTRING)
    set(${variable_name}
        "${vnm_terminal_origin_actual}"
        CACHE PATH
        "${vnm_terminal_origin_help}"
        FORCE)
endfunction()
