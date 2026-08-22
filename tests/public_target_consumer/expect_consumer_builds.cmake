foreach(required_var IN ITEMS
    package_binary_dir
    install_dir
    consumer_source_dir
    consumer_binary_dir)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing required public target consumer variable: ${required_var}")
    endif()
endforeach()

# The library target was already built by the suite build that produced
# package_binary_dir, so this driver installs that build rather than re-driving
# a nested cmake --build (which cannot re-resolve some generators' toolchains).
set(install_args
    --install "${package_binary_dir}"
    --prefix "${install_dir}")

if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND install_args
        --config "${install_config}")
endif()

file(REMOVE_RECURSE
    "${install_dir}"
    "${consumer_binary_dir}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${install_args}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install vnm_terminal for the public target consumer.\n"
        "${install_stdout}${install_stderr}")
endif()

function(resolve_package_dir config_name forwarded_dir output_var)
    file(GLOB_RECURSE config_paths
        LIST_DIRECTORIES FALSE
        "${install_dir}/${config_name}")
    list(LENGTH config_paths config_count)
    if(config_count GREATER 1)
        message(FATAL_ERROR
            "Expected at most one staged ${config_name}, found ${config_count} under "
            "${install_dir}")
    endif()

    if(config_count EQUAL 1)
        list(GET config_paths 0 config_path)
        get_filename_component(package_dir "${config_path}" DIRECTORY)
    else()
        if("${forwarded_dir}" STREQUAL "")
            message(FATAL_ERROR
                "Expected staged ${config_name} or a forwarded package directory")
        endif()
        if(NOT EXISTS "${forwarded_dir}/${config_name}")
            message(FATAL_ERROR
                "Forwarded package directory does not contain ${config_name}: "
                "${forwarded_dir}")
        endif()
        file(REAL_PATH "${forwarded_dir}" package_dir)
    endif()

    set(${output_var} "${package_dir}" PARENT_SCOPE)
endfunction()

resolve_package_dir(
    vnm_terminalConfig.cmake "" vnm_terminal_package_dir)
resolve_package_dir(
    vnm_terminal_surfaceConfig.cmake
    "${vnm_terminal_surface_dir}"
    vnm_terminal_surface_package_dir)
resolve_package_dir(
    vnm_qt_dispatchConfig.cmake
    "${vnm_qt_dispatch_dir}"
    vnm_qt_dispatch_package_dir)

set(configure_args)
if(DEFINED generator AND NOT "${generator}" STREQUAL "")
    list(APPEND configure_args -G "${generator}")
endif()

if(DEFINED generator_platform AND NOT "${generator_platform}" STREQUAL "")
    list(APPEND configure_args -A "${generator_platform}")
endif()

if(DEFINED generator_toolset AND NOT "${generator_toolset}" STREQUAL "")
    list(APPEND configure_args -T "${generator_toolset}")
endif()

if(DEFINED make_program AND NOT "${make_program}" STREQUAL "")
    list(APPEND configure_args "-DCMAKE_MAKE_PROGRAM=${make_program}")
endif()

set(single_config_generator ON)
if(DEFINED generator AND
    "${generator}" MATCHES "Visual Studio|Xcode|Multi-Config")
    set(single_config_generator OFF)
endif()

if(single_config_generator AND
    DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${install_config}")
endif()

if(DEFINED qt6_dir AND NOT "${qt6_dir}" STREQUAL "")
    list(APPEND configure_args "-DQt6_DIR=${qt6_dir}")
endif()
if(DEFINED vnm_msdf_text_dir AND NOT "${vnm_msdf_text_dir}" STREQUAL "")
    list(APPEND configure_args "-Dvnm_msdf_text_DIR:PATH=${vnm_msdf_text_dir}")
endif()

set(consumer_prefix_paths "${install_dir}")
if(DEFINED vnm_msdf_text_dependency_prefixes AND
    NOT "${vnm_msdf_text_dependency_prefixes}" STREQUAL "")
    list(APPEND consumer_prefix_paths ${vnm_msdf_text_dependency_prefixes})
endif()

list(APPEND configure_args
    "-Dvnm_terminal_DIR:PATH=${vnm_terminal_package_dir}"
    "-Dvnm_terminal_surface_DIR:PATH=${vnm_terminal_surface_package_dir}"
    "-Dvnm_qt_dispatch_DIR:PATH=${vnm_qt_dispatch_package_dir}"
    "-DCMAKE_FIND_USE_PACKAGE_ROOT_PATH=FALSE"
    "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE"
    "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE"
    "-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE"
    "-DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=TRUE")

# Keep the prefix list directly quoted so its semicolons remain one child
# configure argument instead of becoming configure_args list separators.
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${consumer_source_dir}"
        -B "${consumer_binary_dir}"
        "-DCMAKE_PREFIX_PATH:PATH=${consumer_prefix_paths}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Public target consumer configure failed.\n"
        "${configure_stdout}${configure_stderr}")
endif()

function(expect_consumer_package_dir variable_name expected_dir)
    set(consumer_cache "${consumer_binary_dir}/CMakeCache.txt")
    file(STRINGS "${consumer_cache}" package_cache_entries
        REGEX "^${variable_name}:PATH=")
    list(LENGTH package_cache_entries package_cache_entry_count)
    if(NOT package_cache_entry_count EQUAL 1)
        message(FATAL_ERROR
            "Public target consumer cache does not contain exactly one "
            "${variable_name} entry:\n"
            "  ${consumer_cache}")
    endif()
    list(GET package_cache_entries 0 package_cache_entry)
    string(REGEX REPLACE "^[^=]*=" "" resolved_package_dir
        "${package_cache_entry}")
    file(REAL_PATH "${resolved_package_dir}" resolved_package_dir)
    file(REAL_PATH "${expected_dir}" expected_dir)
    if(NOT resolved_package_dir STREQUAL expected_dir)
        message(FATAL_ERROR
            "Public target consumer resolved ${variable_name} outside its "
            "explicit package directory:\n"
            "  expected=${expected_dir}\n"
            "  actual=${resolved_package_dir}")
    endif()
endfunction()

expect_consumer_package_dir(
    vnm_terminal_DIR "${vnm_terminal_package_dir}")
expect_consumer_package_dir(
    vnm_terminal_surface_DIR "${vnm_terminal_surface_package_dir}")
expect_consumer_package_dir(
    vnm_qt_dispatch_DIR "${vnm_qt_dispatch_package_dir}")
if(DEFINED vnm_msdf_text_dir AND NOT "${vnm_msdf_text_dir}" STREQUAL "")
    expect_consumer_package_dir(
        vnm_msdf_text_DIR "${vnm_msdf_text_dir}")
endif()

set(consumer_build_args
    --build "${consumer_binary_dir}")
if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND consumer_build_args
        --config "${install_config}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${consumer_build_args}
    RESULT_VARIABLE consumer_build_result
    OUTPUT_VARIABLE consumer_build_stdout
    ERROR_VARIABLE consumer_build_stderr)

if(NOT consumer_build_result EQUAL 0)
    message(FATAL_ERROR
        "Public target consumer failed to link the installed app support.\n"
        "${consumer_build_stdout}${consumer_build_stderr}")
endif()

message(STATUS
    "Public target consumer built against the installed vnm_terminal package.")
