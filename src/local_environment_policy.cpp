#include "local_environment_policy.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>

#if defined(_WIN32)
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

namespace vnm_terminal::local_environment_policy {
namespace {

constexpr std::array<std::string_view, 12> k_reserved_names{
    "VNM_CONTROL_ENDPOINT",
    "VNM_CONTROL_TOKEN",
    "VNM_OWNER_ENDPOINT",
    "VNM_OWNER_TOKEN",
    "VNM_RELAY_ENDPOINT",
    "VNM_RELAY_TOKEN",
    "VNM_BOOTSTRAP_ENDPOINT",
    "VNM_BOOTSTRAP_TOKEN",
    "VNM_INVITATION_TOKEN",
    "VNM_AUTHORIZATION_TOKEN",
    "VNM_WORKER_CONTROL_ENDPOINT",
    "VNM_WORKER_CONTROL_TOKEN",
};

enum class Environment_provenance
{
    EXPLICIT,
    AMBIENT,
};

bool contains_nul(std::string_view text)
{
    return text.find('\0') != std::string_view::npos;
}

bool is_windows_drive_pseudo_variable(std::string_view name)
{
    const bool has_drive_letter =
        name.size() >= 2 &&
        ((name[1] >= 'A' && name[1] <= 'Z') ||
         (name[1] >= 'a' && name[1] <= 'z'));
    return
        name.size() == 3 &&
        name[0] == '=' &&
        has_drive_letter &&
        name[2] == ':';
}

bool is_valid_utf8(std::string_view text)
{
    const auto is_continuation = [](unsigned char byte) {
        return byte >= 0x80 && byte <= 0xbf;
    };

    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        if (first >= 0xc2 && first <= 0xdf) {
            if (index + 1 >= text.size() ||
                !is_continuation(
                    static_cast<unsigned char>(text[index + 1])))
            {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xe0 && first <= 0xef) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            const unsigned char third =
                static_cast<unsigned char>(text[index + 2]);
            bool valid_second = is_continuation(second);
            if (first == 0xe0) {
                valid_second = second >= 0xa0 && second <= 0xbf;
            }
            else
            if (first == 0xed) {
                valid_second = second >= 0x80 && second <= 0x9f;
            }
            if (!valid_second || !is_continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xf0 && first <= 0xf4) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            bool valid_second = is_continuation(second);
            if (first == 0xf0) {
                valid_second = second >= 0x90 && second <= 0xbf;
            }
            else
            if (first == 0xf4) {
                valid_second = second >= 0x80 && second <= 0x8f;
            }
            if (!valid_second ||
                !is_continuation(
                    static_cast<unsigned char>(text[index + 2])) ||
                !is_continuation(
                    static_cast<unsigned char>(text[index + 3])))
            {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }
    return true;
}

#if defined(_WIN32)
std::optional<std::wstring> utf8_to_windows_text(std::string_view text)
{
    if (text.empty()) {
        return std::wstring{};
    }
    if (text.size() >
        static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }

    const int required_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required_size <= 0) {
        return std::nullopt;
    }

    std::wstring converted(static_cast<std::size_t>(required_size), L'\0');
    const int converted_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        converted.data(),
        required_size);
    if (converted_size != required_size) {
        return std::nullopt;
    }
    return converted;
}

bool is_valid_windows_name_encoding(std::string_view name)
{
    return is_valid_utf8(name) && utf8_to_windows_text(name).has_value();
}

bool windows_names_equal(std::string_view left, std::string_view right)
{
    if (left == right) {
        return true;
    }

    const std::optional<std::wstring> left_text = utf8_to_windows_text(left);
    const std::optional<std::wstring> right_text = utf8_to_windows_text(right);
    if (!left_text || !right_text) {
        return false;
    }

    return CompareStringOrdinal(
               left_text->data(),
               static_cast<int>(left_text->size()),
               right_text->data(),
               static_cast<int>(right_text->size()),
               TRUE) == CSTR_EQUAL;
}
#else
bool is_valid_windows_name_encoding(std::string_view name)
{
    return is_valid_utf8(name);
}

bool windows_names_equal(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }

    return std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [](char left_character, char right_character) {
            unsigned char left_byte =
                static_cast<unsigned char>(left_character);
            unsigned char right_byte =
                static_cast<unsigned char>(right_character);
            if (left_byte >= 'A' && left_byte <= 'Z') {
                left_byte = static_cast<unsigned char>(left_byte - 'A' + 'a');
            }
            if (right_byte >= 'A' && right_byte <= 'Z') {
                right_byte = static_cast<unsigned char>(right_byte - 'A' + 'a');
            }
            return left_byte == right_byte;
        });
}
#endif

bool environment_names_equal(
    std::string_view left,
    std::string_view right,
    Environment_platform platform)
{
    if (platform == Environment_platform::POSIX) {
        return left == right;
    }
    return windows_names_equal(left, right);
}

bool is_valid_base_name(
    std::string_view name,
    Environment_platform platform)
{
    if (!name.empty() && name.front() == '=') {
        return
            platform == Environment_platform::WINDOWS &&
            is_windows_drive_pseudo_variable(name);
    }
    if (name.empty() || contains_nul(name) ||
        name.find('=') != std::string_view::npos)
    {
        return false;
    }
    return
        platform != Environment_platform::WINDOWS ||
        is_valid_windows_name_encoding(name);
}

bool is_reserved_name(
    std::string_view name,
    Environment_platform platform)
{
    return std::any_of(
        k_reserved_names.begin(),
        k_reserved_names.end(),
        [name, platform](std::string_view reserved_name) {
            return environment_names_equal(name, reserved_name, platform);
        });
}

bool strips_unsupported_windows_pseudo_variable(
    std::string_view name,
    Environment_platform platform,
    Environment_provenance provenance)
{
    return
        provenance == Environment_provenance::AMBIENT &&
        platform == Environment_platform::WINDOWS &&
        !name.empty() &&
        name.front() == '=' &&
        !contains_nul(name) &&
        is_valid_windows_name_encoding(name) &&
        !is_windows_drive_pseudo_variable(name);
}

Environment_sanitization_result sanitize_base_environment(
    std::span<const Environment_entry> base_entries,
    Environment_platform platform,
    Environment_provenance provenance)
{
    std::vector<bool> stripped_entries(base_entries.size(), false);
    bool accepted = true;
    for (std::size_t index = 0; index < base_entries.size(); ++index) {
        const Environment_entry& entry = base_entries[index];
        if (strips_unsupported_windows_pseudo_variable(
                entry.name,
                platform,
                provenance))
        {
            stripped_entries[index] = true;
        }
        else
        if (!is_valid_base_name(entry.name, platform)) {
            accepted = false;
        }
        if (contains_nul(entry.value)) {
            accepted = false;
        }

        for (std::size_t previous = 0; previous < index; ++previous) {
            if (environment_names_equal(
                    entry.name,
                    base_entries[previous].name,
                    platform))
            {
                accepted = false;
                break;
            }
        }
    }

    if (!accepted) {
        return {};
    }

    Environment_sanitization_result result;
    result.entries.reserve(base_entries.size());
    for (std::size_t index = 0; index < base_entries.size(); ++index) {
        const Environment_entry& entry = base_entries[index];
        if (!stripped_entries[index] &&
            !is_reserved_name(entry.name, platform))
        {
            result.entries.push_back(entry);
        }
    }
    result.accepted = true;
    return result;
}

} // namespace

Environment_sanitization_result sanitize_explicit_base_environment(
    std::span<const Environment_entry> base_entries,
    Environment_platform platform)
{
    return sanitize_base_environment(
        base_entries,
        platform,
        Environment_provenance::EXPLICIT);
}

Environment_sanitization_result sanitize_ambient_environment(
    std::span<const Environment_entry> ambient_entries,
    Environment_platform platform)
{
    return sanitize_base_environment(
        ambient_entries,
        platform,
        Environment_provenance::AMBIENT);
}

} // namespace vnm_terminal::local_environment_policy
