function(vnm_terminal_script_command_args out_var)
    set(separator_index -1)
    math(EXPR last_index "${CMAKE_ARGC} - 1")
    foreach(index RANGE 0 ${last_index})
        if(separator_index LESS 0 AND "${CMAKE_ARGV${index}}" STREQUAL "--")
            set(separator_index ${index})
        endif()
    endforeach()

    if(separator_index LESS 0)
        message(FATAL_ERROR "expected process command after --")
    endif()

    math(EXPR command_start "${separator_index} + 1")
    if(command_start GREATER last_index)
        message(FATAL_ERROR "expected process command after --")
    endif()

    set(command_args)
    foreach(index RANGE ${command_start} ${last_index})
        set(command_arg "${CMAKE_ARGV${index}}")
        string(REPLACE ";" "\\;" command_arg "${command_arg}")
        list(APPEND command_args "${command_arg}")
    endforeach()

    set(${out_var} "${command_args}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_read_json_field out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error GET "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON field '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_read_json_type out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error TYPE "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON field '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_read_json_length out_value json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error LENGTH "${json_text}" ${json_path})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} is missing JSON object '${json_path_text}': ${json_error}")
    endif()
    set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_expect_json_missing json_text source_path)
    set(json_path ${ARGN})
    string(JSON value ERROR_VARIABLE json_error TYPE "${json_text}" ${json_path})
    if(json_error STREQUAL "NOTFOUND")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be absent")
    endif()
endfunction()

function(vnm_terminal_expect_json_counter json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_field(counter_value "${json_text}" "${source_path}" ${json_path})
    if(NOT counter_value MATCHES "^[0-9]+$")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be an integer counter, "
            "got ${counter_value}")
    endif()
endfunction()

function(vnm_terminal_expect_json_boolean json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_type(field_type "${json_text}" "${source_path}" ${json_path})
    if(NOT field_type STREQUAL "BOOLEAN")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be a boolean, "
            "got ${field_type}")
    endif()
endfunction()

function(vnm_terminal_expect_json_number json_text source_path)
    set(json_path ${ARGN})
    vnm_terminal_read_json_type(field_type "${json_text}" "${source_path}" ${json_path})
    if(NOT field_type STREQUAL "NUMBER")
        list(JOIN json_path "." json_path_text)
        message(FATAL_ERROR
            "${source_path} JSON field '${json_path_text}' should be a number, "
            "got ${field_type}")
    endif()
endfunction()

function(vnm_terminal_validate_positive_int32 option_name option_value)
    if(NOT "${option_value}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${option_name} must be an integer in 1..2147483647")
    endif()

    string(LENGTH "${option_value}" option_length)
    if(option_length GREATER 10 OR
        (option_length EQUAL 10 AND "${option_value}" STRGREATER "2147483647"))
        message(FATAL_ERROR "${option_name} must be an integer in 1..2147483647")
    endif()
endfunction()
