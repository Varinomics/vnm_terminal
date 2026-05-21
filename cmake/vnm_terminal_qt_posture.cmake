set(VNM_TERMINAL_QT_LICENSE_ROUTE "lgpl_dynamic" CACHE STRING
    "Qt license route: lgpl_dynamic or commercial")
set_property(CACHE VNM_TERMINAL_QT_LICENSE_ROUTE PROPERTY STRINGS
    "lgpl_dynamic"
    "commercial")

function(vnm_terminal_allowed_qt_targets out_var)
    set(${out_var}
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        PARENT_SCOPE)
endfunction()

function(vnm_terminal_validate_qt_target target)
    vnm_terminal_allowed_qt_targets(allowed_qt_targets)

    if(target MATCHES "^\\$<.*\\$<TARGET_OBJECTS:\\$<TARGET_NAME:vnm_terminal_resources_[0-9]+>>>$")
        return()
    endif()

    if(target MATCHES "\\$<.*Qt6::")
        message(FATAL_ERROR
            "Qt generator expression ${target} is outside the Qt module allowlist")
    endif()

    if(NOT target MATCHES "^Qt6::")
        return()
    endif()

    if(NOT target IN_LIST allowed_qt_targets)
        message(FATAL_ERROR
            "Qt target ${target} is outside the Qt module allowlist")
    endif()
endfunction()

function(vnm_terminal_validate_qt_posture)
    set(options)
    set(one_value_args)
    set(multi_value_args DIRECT_TARGETS)
    cmake_parse_arguments(PARSE_ARGV 0 posture
        "${options}" "${one_value_args}" "${multi_value_args}")

    if(posture_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unexpected Qt posture arguments: ${posture_UNPARSED_ARGUMENTS}")
    endif()

    set(allowed_routes
        "lgpl_dynamic"
        "commercial")

    if(NOT VNM_TERMINAL_QT_LICENSE_ROUTE IN_LIST allowed_routes)
        message(FATAL_ERROR
            "VNM_TERMINAL_QT_LICENSE_ROUTE must be lgpl_dynamic or commercial")
    endif()

    vnm_terminal_allowed_qt_targets(allowed_qt_targets)

    foreach(target IN LISTS posture_DIRECT_TARGETS)
        vnm_terminal_validate_qt_target(${target})
    endforeach()

    if(VNM_TERMINAL_QT_LICENSE_ROUTE STREQUAL "commercial")
        return()
    endif()

    if(DEFINED QT_FEATURE_shared)
        if(NOT QT_FEATURE_shared)
            message(FATAL_ERROR
                "The lgpl_dynamic Qt route requires a shared Qt build")
        endif()
    endif()

    foreach(target IN LISTS allowed_qt_targets)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "Required Qt target ${target} is missing")
        endif()

        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "SHARED_LIBRARY")
            message(FATAL_ERROR
                "The lgpl_dynamic Qt route requires ${target} to be a shared library")
        endif()
    endforeach()
endfunction()

function(vnm_terminal_validate_qt_link_interface target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "Target ${target} does not exist")
    endif()

    foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(linked_libraries ${target} ${property})
        if(NOT linked_libraries OR linked_libraries STREQUAL "linked_libraries-NOTFOUND")
            continue()
        endif()

        foreach(linked_library IN LISTS linked_libraries)
            vnm_terminal_validate_qt_target(${linked_library})
        endforeach()
    endforeach()
endfunction()
