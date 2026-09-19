function(vnm_terminal_environment_policy_make_available)
    add_library(vnm_terminal_environment_policy INTERFACE)
    add_library(
        vnm_terminal::vnm_terminal_environment_policy
        ALIAS vnm_terminal_environment_policy)
    target_sources(vnm_terminal_environment_policy INTERFACE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/local_environment_policy.cpp")

    set_property(TARGET vnm_terminal_environment_policy PROPERTY
        VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER "terminal-local")
    set(VNM_TERMINAL_ENVIRONMENT_POLICY_PROVIDER
        "terminal-local"
        PARENT_SCOPE)
endfunction()
