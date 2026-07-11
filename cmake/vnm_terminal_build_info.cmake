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

function(vnm_terminal_build_provenance_text out_var terminal_dir surface_dir chrome_dir)
    string(TIMESTAMP build_date "%Y-%m-%d %H:%M:%S UTC" UTC)

    vnm_terminal_git_summary(
        terminal_commit
        terminal_modified
        "${terminal_dir}")
    vnm_terminal_git_summary(
        surface_commit
        surface_modified
        "${surface_dir}")
    vnm_terminal_git_summary(
        chrome_commit
        chrome_modified
        "${chrome_dir}")

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
    vnm_terminal_build_info_line(
        chrome_line
        "vnm_qml_chrome"
        "${chrome_commit}"
        "${chrome_modified}")

    set(text "Build date: ${build_date}\n${terminal_line}\n${surface_line}\n${chrome_line}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

function(vnm_terminal_cxx_string_literal out_var value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")

    set(${out_var} "\"${escaped}\"" PARENT_SCOPE)
endfunction()
