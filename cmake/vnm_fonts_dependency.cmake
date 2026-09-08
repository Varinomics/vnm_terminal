set(VNM_FONTS_SOURCE_DIR "" CACHE PATH
    "Path to a source checkout of vnm_fonts.")

# Two font files that declare the same family name do not shadow one another:
# they merge into one font-database entry, after which glyph lookup and
# rasterisation can be served from different files. vnm_fonts ships the fonts
# byte-verbatim and marks the family name on the way into the database, so the
# face this application draws is the file it carries rather than whatever the
# host has installed under the same name.
#
# Reuse the public target whether a source dependency or an installed package
# supplied it earlier in the configure.
if(TARGET vnm::fonts)
    vnm_terminal_adopt_existing_target_source(vnm::fonts VNM_FONTS_SOURCE_DIR)
    return()
endif()

if(NOT VNM_FONTS_SOURCE_DIR)
    get_filename_component(vnm_terminal_default_fonts_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/../vnm_fonts"
        ABSOLUTE)
    if(EXISTS "${vnm_terminal_default_fonts_dir}/CMakeLists.txt")
        set(VNM_FONTS_SOURCE_DIR
            "${vnm_terminal_default_fonts_dir}"
            CACHE PATH
            "Path to a source checkout of vnm_fonts."
            FORCE)
    endif()
endif()

# No EXCLUDE_FROM_ALL on the declaration below: FetchContent passes that form
# through to add_subdirectory, which discards a subdirectory's install rules
# outright, and the licences would never reach the package.
include(FetchContent)
if(VNM_FONTS_SOURCE_DIR)
    if(NOT EXISTS "${VNM_FONTS_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "VNM_FONTS_SOURCE_DIR does not contain a vnm_fonts "
            "CMakeLists.txt:\n  ${VNM_FONTS_SOURCE_DIR}")
    endif()
    FetchContent_Declare(vnm_terminal_fonts
        SOURCE_DIR "${VNM_FONTS_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/vnm_fonts-build")
else()
    FetchContent_Declare(vnm_terminal_fonts
        GIT_REPOSITORY "https://github.com/Varinomics/vnm_fonts.git"
        GIT_TAG        master
        GIT_SHALLOW    FALSE
        BINARY_DIR     "${CMAKE_BINARY_DIR}/_deps/vnm_fonts-build")
endif()
FetchContent_MakeAvailable(vnm_terminal_fonts)

# vnm_fonts defines its library only when it finds Qt, so that a consumer that
# wants nothing but the font directory can configure it without Qt at all. This
# build links the library, and Qt is found above, so an absent target here means
# the selected source is not a vnm_fonts that supplies one.
if(NOT TARGET vnm::fonts)
    message(FATAL_ERROR
        "vnm_fonts is unavailable after dependency discovery: the selected "
        "source did not provide the vnm::fonts target.")
endif()
