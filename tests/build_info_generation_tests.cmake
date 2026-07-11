if(NOT VNM_TERMINAL_SOURCE_ROOT OR NOT VNM_TERMINAL_TEST_ROOT)
    message(FATAL_ERROR "Source and test roots are required")
endif()

file(REMOVE_RECURSE "${VNM_TERMINAL_TEST_ROOT}")
file(MAKE_DIRECTORY "${VNM_TERMINAL_TEST_ROOT}")

function(create_test_repo name out_dir)
    set(repo_dir "${VNM_TERMINAL_TEST_ROOT}/${name}")
    file(MAKE_DIRECTORY "${repo_dir}")
    file(WRITE "${repo_dir}/tracked.txt" "tracked\n")
    execute_process(
        COMMAND git init --quiet "${repo_dir}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git -C "${repo_dir}" config user.email "build-info-test@invalid"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git -C "${repo_dir}" config user.name "Build info test"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git -C "${repo_dir}" add tracked.txt
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git -C "${repo_dir}" commit --quiet -m initial
        COMMAND_ERROR_IS_FATAL ANY)
    set(${out_dir} "${repo_dir}" PARENT_SCOPE)
endfunction()

function(generate_build_info)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DVNM_TERMINAL_SOURCE_DIR=${terminal_repo}"
            "-DVNM_TERMINAL_SURFACE_SOURCE_DIR=${surface_repo}"
            "-DVNM_QML_CHROME_SOURCE_DIR=${chrome_repo}"
            "-DVNM_TERMINAL_BUILD_DATE=2026-07-11 12:00:00 UTC"
            "-DVNM_TERMINAL_BUILD_INFO_OUTPUT=${output_header}"
            -P "${VNM_TERMINAL_SOURCE_ROOT}/cmake/vnm_terminal_generate_build_info.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

create_test_repo(terminal terminal_repo)
create_test_repo(surface surface_repo)
create_test_repo(chrome chrome_repo)
set(output_header "${VNM_TERMINAL_TEST_ROOT}/generated/vnm_terminal_build_info.h")

generate_build_info()
file(SHA256 "${output_header}" first_hash)
file(TIMESTAMP "${output_header}" first_timestamp "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
generate_build_info()
file(SHA256 "${output_header}" second_hash)
file(TIMESTAMP "${output_header}" second_timestamp "%s")
if(NOT first_hash STREQUAL second_hash OR NOT first_timestamp STREQUAL second_timestamp)
    message(FATAL_ERROR "No-change generation rewrote the compiled provenance header")
endif()

file(WRITE "${surface_repo}/untracked.txt" "untracked\n")
generate_build_info()
file(READ "${output_header}" dirty_header)
if(NOT dirty_header MATCHES "vnm_terminal_surface: [^\\\\n]+ \\(modified files present\\)")
    message(FATAL_ERROR "Untracked Surface files were not reported")
endif()

file(REMOVE "${surface_repo}/untracked.txt")
generate_build_info()
file(READ "${output_header}" clean_header)
if(clean_header MATCHES "modified files present")
    message(FATAL_ERROR "Clean repositories were reported as modified")
endif()
