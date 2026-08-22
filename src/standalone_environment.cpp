#include "standalone_environment.h"

#if !defined(VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK)
    #error "The selected environment-policy target must define its provider"
#elif VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK
    #include <environment_policy/vnm_environment_policy.h>
#else
    #include "local_environment_policy.h"
#endif

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cstddef>
#include <string>

namespace vnm_terminal::terminal_app {
namespace {

#if VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK
namespace selected_policy = vnm::environment_policy;
#else
namespace selected_policy = vnm_terminal::local_environment_policy;
#endif

selected_policy::Environment_platform environment_platform()
{
#if defined(Q_OS_WIN)
    return selected_policy::Environment_platform::WINDOWS;
#else
    return selected_policy::Environment_platform::POSIX;
#endif
}

std::string utf8_string(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString utf8_qstring(const std::string& value)
{
    return QString::fromUtf8(
        value.data(),
        static_cast<qsizetype>(value.size()));
}

enum class Sanitizer_kind
{
    EXPLICIT,
    AMBIENT,
};

selected_policy::Environment_sanitization_result sanitize_selected_environment(
    std::span<const selected_policy::Environment_entry> entries,
    Sanitizer_kind sanitizer_kind)
{
    if (sanitizer_kind == Sanitizer_kind::EXPLICIT) {
#if VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK
        return selected_policy::sanitize_explicit_base_environment(
            entries,
            environment_platform(),
            {});
#else
        return selected_policy::sanitize_explicit_base_environment(
            entries,
            environment_platform());
#endif
    }

#if VNM_TERMINAL_ENVIRONMENT_POLICY_USE_FRAMEWORK
    return selected_policy::sanitize_ambient_environment(
        entries,
        environment_platform(),
        {});
#else
    return selected_policy::sanitize_ambient_environment(
        entries,
        environment_platform());
#endif
}

std::optional<std::vector<Terminal_environment_entry>> sanitize_environment(
    std::span<const Terminal_environment_entry> captured_environment,
    Sanitizer_kind sanitizer_kind)
{
    std::vector<selected_policy::Environment_entry> entries;
    entries.reserve(captured_environment.size());
    for (const Terminal_environment_entry& entry : captured_environment) {
        entries.push_back({
            utf8_string(entry.name),
            utf8_string(entry.value),
        });
    }

    selected_policy::Environment_sanitization_result sanitized =
        sanitize_selected_environment(entries, sanitizer_kind);
    if (!sanitized.accepted) {
        return std::nullopt;
    }

    std::vector<Terminal_environment_entry> result;
    result.reserve(sanitized.entries.size());
    for (selected_policy::Environment_entry& entry : sanitized.entries) {
        result.push_back({
            utf8_qstring(entry.name),
            utf8_qstring(entry.value),
        });
    }
    return result;
}

} // namespace

std::optional<std::vector<Terminal_environment_entry>>
sanitize_standalone_base_environment(
    std::span<const Terminal_environment_entry> captured_environment)
{
    return sanitize_environment(
        captured_environment,
        Sanitizer_kind::EXPLICIT);
}

std::optional<std::vector<Terminal_environment_entry>>
sanitize_standalone_ambient_environment(
    std::span<const Terminal_environment_entry> captured_environment)
{
    return sanitize_environment(
        captured_environment,
        Sanitizer_kind::AMBIENT);
}

std::vector<Terminal_environment_entry>
capture_standalone_ambient_environment()
{
    const QProcessEnvironment captured = QProcessEnvironment::systemEnvironment();
    const QStringList names = captured.keys();
    std::vector<Terminal_environment_entry> result;
    result.reserve(static_cast<std::size_t>(names.size()));
    for (const QString& name : names) {
        result.push_back({name, captured.value(name)});
    }
    return result;
}

} // namespace vnm_terminal::terminal_app
