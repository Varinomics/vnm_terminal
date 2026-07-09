function(vnm_terminal_git_path out_var repo_dir git_path)
    if(IS_ABSOLUTE "${git_path}")
        set(abs_path "${git_path}")
    else()
        get_filename_component(abs_path "${repo_dir}/${git_path}" ABSOLUTE)
    endif()

    set(${out_var} "${abs_path}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_register_git_configure_inputs repo_dir)
    execute_process(
        COMMAND git -C "${repo_dir}" rev-parse --is-inside-work-tree
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE inside_work_tree
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT git_result EQUAL 0 OR NOT inside_work_tree STREQUAL "true")
        return()
    endif()

    execute_process(
        COMMAND git -C "${repo_dir}" rev-parse --git-dir
        RESULT_VARIABLE git_dir_result
        OUTPUT_VARIABLE git_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${repo_dir}" rev-parse --git-common-dir
        RESULT_VARIABLE git_common_dir_result
        OUTPUT_VARIABLE git_common_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT git_dir_result EQUAL 0 OR NOT git_common_dir_result EQUAL 0)
        return()
    endif()

    vnm_terminal_git_path(git_dir_abs "${repo_dir}" "${git_dir}")
    vnm_terminal_git_path(git_common_dir_abs "${repo_dir}" "${git_common_dir}")

    set(git_inputs
        "${git_dir_abs}/HEAD"
        "${git_dir_abs}/index")

    if(EXISTS "${git_common_dir_abs}/packed-refs")
        list(APPEND git_inputs "${git_common_dir_abs}/packed-refs")
    endif()

    execute_process(
        COMMAND git -C "${repo_dir}" symbolic-ref -q HEAD
        RESULT_VARIABLE git_ref_result
        OUTPUT_VARIABLE git_ref
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(git_ref_result EQUAL 0 AND NOT git_ref STREQUAL "")
        list(APPEND git_inputs "${git_common_dir_abs}/${git_ref}")
    endif()

    execute_process(
        COMMAND git -C "${repo_dir}" ls-files
        RESULT_VARIABLE git_files_result
        OUTPUT_VARIABLE git_files
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(git_files_result EQUAL 0 AND NOT git_files STREQUAL "")
        string(REPLACE "\n" ";" tracked_files "${git_files}")
        foreach(tracked_file IN LISTS tracked_files)
            list(APPEND git_inputs "${repo_dir}/${tracked_file}")
        endforeach()
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${git_inputs})
endfunction()

function(vnm_terminal_git_summary out_commit out_modified repo_dir)
    set(commit "unknown")
    set(modified OFF)

    if(repo_dir AND EXISTS "${repo_dir}")
        execute_process(
            COMMAND git -C "${repo_dir}" rev-parse --is-inside-work-tree
            RESULT_VARIABLE git_result
            OUTPUT_VARIABLE inside_work_tree
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(git_result EQUAL 0 AND inside_work_tree STREQUAL "true")
            execute_process(
                COMMAND git -C "${repo_dir}" rev-parse --verify HEAD
                RESULT_VARIABLE git_commit_result
                OUTPUT_VARIABLE git_commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(git_commit_result EQUAL 0 AND NOT git_commit STREQUAL "")
                set(commit "${git_commit}")
            endif()

            execute_process(
                COMMAND git -C "${repo_dir}" status --porcelain --untracked-files=no
                RESULT_VARIABLE git_status_result
                OUTPUT_VARIABLE git_status
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(git_status_result EQUAL 0 AND NOT git_status STREQUAL "")
                set(modified ON)
            endif()

            vnm_terminal_register_git_configure_inputs("${repo_dir}")
        endif()
    endif()

    set(${out_commit} "${commit}" PARENT_SCOPE)
    set(${out_modified} "${modified}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_build_info_line out_var component_name commit modified)
    set(line "${component_name}: ${commit}")
    if(modified)
        string(APPEND line " (modified files present)")
    endif()

    set(${out_var} "${line}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_build_provenance_text out_var terminal_dir surface_dir)
    string(TIMESTAMP build_date "%Y-%m-%d %H:%M:%S UTC" UTC)

    vnm_terminal_git_summary(
        terminal_commit
        terminal_modified
        "${terminal_dir}")
    vnm_terminal_git_summary(
        surface_commit
        surface_modified
        "${surface_dir}")

    vnm_terminal_build_info_line(
        terminal_line
        "vnm_terminal"
        "${terminal_commit}"
        "${terminal_modified}")
    vnm_terminal_build_info_line(
        surface_line
        "vnm_terminal_surface"
        "${surface_commit}"
        "${surface_modified}")

    set(text "Build date: ${build_date}\n${terminal_line}\n${surface_line}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_cxx_string_literal out_var value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")

    set(${out_var} "\"${escaped}\"" PARENT_SCOPE)
endfunction()
