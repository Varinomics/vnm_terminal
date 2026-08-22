function(vnm_terminal_environment_policy_make_available)
    add_library(vnm_terminal_local_environment_policy INTERFACE)
    target_sources(vnm_terminal_local_environment_policy INTERFACE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/local_environment_policy.cpp")
    target_compile_definitions(vnm_terminal_local_environment_policy INTERFACE
        VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK=0)

    add_library(vnm_terminal_environment_policy INTERFACE)
    add_library(
        vnm_terminal::vnm_terminal_environment_policy
        ALIAS vnm_terminal_environment_policy)

    if(TARGET vnm_framework::vnm_environment_policy)
        set(provider "vnm_framework")
        target_link_libraries(vnm_terminal_environment_policy INTERFACE
            vnm_framework::vnm_environment_policy)
        target_compile_definitions(vnm_terminal_environment_policy INTERFACE
            VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK=1)
    else()
        set(provider "terminal-local substitute")
        target_link_libraries(vnm_terminal_environment_policy INTERFACE
            vnm_terminal_local_environment_policy)
    endif()

    set_property(TARGET vnm_terminal_environment_policy PROPERTY
        VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER "${provider}")
    set(VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER
        "${provider}"
        PARENT_SCOPE)
endfunction()
