include("${CMAKE_CURRENT_LIST_DIR}/vnm_cmake_dependency.cmake")
vnm_acquire_owned_dependency(NAME vnm_fonts
    GIT_REPOSITORY https://github.com/Varinomics/vnm_fonts.git
    TARGET vnm::fonts)
