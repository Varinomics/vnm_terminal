# CPack runs this after installation and before creating the DEB/RPM archive.
find_program(vnm_terminal_python NAMES python3 REQUIRED)
execute_process(
    COMMAND "${vnm_terminal_python}"
        "${CMAKE_CURRENT_LIST_DIR}/finalize_linux_payload.py"
        --cpack-root "${CPACK_TEMPORARY_DIRECTORY}"
    COMMAND_ERROR_IS_FATAL ANY
)
