#pragma once

#include <span>
#include <string>
#include <vector>

namespace vnm_terminal::local_environment_policy {

enum class Environment_platform
{
    WINDOWS,
    POSIX,
};

struct Environment_entry
{
    std::string name;
    std::string value;
};

struct Environment_sanitization_result
{
    bool accepted = false;
    std::vector<Environment_entry> entries;
};

Environment_sanitization_result sanitize_explicit_base_environment(
    std::span<const Environment_entry> base_entries,
    Environment_platform platform);

Environment_sanitization_result sanitize_ambient_environment(
    std::span<const Environment_entry> ambient_entries,
    Environment_platform platform);

} // namespace vnm_terminal::local_environment_policy
