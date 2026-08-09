if(NOT DEFINED VNM_TERMINAL_SOURCE_ROOT)
    message(FATAL_ERROR "VNM_TERMINAL_SOURCE_ROOT is required")
endif()

function(check_wix_bitmap filename expected_height_hex expected_dimensions)
    set(bitmap
        "${VNM_TERMINAL_SOURCE_ROOT}/packaging/windows/${filename}")
    if(NOT EXISTS "${bitmap}")
        message(FATAL_ERROR "WiX bitmap is missing: ${bitmap}")
    endif()

    file(READ "${bitmap}" bitmap_signature OFFSET 0 LIMIT 2 HEX)
    file(READ "${bitmap}" bitmap_width OFFSET 18 LIMIT 4 HEX)
    file(READ "${bitmap}" bitmap_height OFFSET 22 LIMIT 4 HEX)

    if(NOT bitmap_signature STREQUAL "424d")
        message(FATAL_ERROR "WiX asset is not a BMP file: ${bitmap}")
    endif()
    if(NOT bitmap_width STREQUAL "ed010000" OR
        NOT bitmap_height STREQUAL "${expected_height_hex}")
        message(FATAL_ERROR
            "${filename} must remain exactly ${expected_dimensions} pixels")
    endif()
endfunction()

check_wix_bitmap(
    vnm_installer_dialog.bmp
    "38010000"
    "493x312")
check_wix_bitmap(
    vnm_installer_banner.bmp
    "3a000000"
    "493x58")
