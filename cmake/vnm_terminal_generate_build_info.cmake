include("${CMAKE_CURRENT_LIST_DIR}/vnm_terminal_build_info.cmake")

vnm_terminal_build_provenance_text(
    vnm_terminal_build_provenance_text
    "${VNM_TERMINAL_SOURCE_DIR}"
    "${VNM_TERMINAL_SURFACE_SOURCE_DIR}"
    "${VNM_QML_CHROME_SOURCE_DIR}")
vnm_terminal_cxx_string_literal(
    vnm_terminal_build_provenance_text_literal
    "${vnm_terminal_build_provenance_text}")

get_filename_component(output_dir "${VNM_TERMINAL_BUILD_INFO_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/vnm_terminal_build_info.h.in"
    "${VNM_TERMINAL_BUILD_INFO_OUTPUT}"
    @ONLY)
