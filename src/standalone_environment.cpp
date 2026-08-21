#include "standalone_environment.h"

#include <environment_policy/vnm_environment_policy.h>

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cstddef>
#include <string>

namespace vnm_terminal::terminal_app {
namespace {

vnm::environment_policy::Environment_platform environment_platform()
{
#if defined(Q_OS_WIN)
    return vnm::environment_policy::Environment_platform::WINDOWS;
#else
    return vnm::environment_policy::Environment_platform::POSIX;
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

} // namespace

std::optional<std::vector<Terminal_environment_entry>>
sanitize_standalone_base_environment(
    std::span<const Terminal_environment_entry> captured_environment)
{
    std::vector<vnm::environment_policy::Environment_entry> entries;
    entries.reserve(captured_environment.size());
    for (const Terminal_environment_entry& entry : captured_environment) {
        entries.push_back({
            utf8_string(entry.name),
            utf8_string(entry.value),
        });
    }

    vnm::environment_policy::Environment_sanitization_result sanitized =
        vnm::environment_policy::sanitize_explicit_base_environment(
            entries,
            environment_platform());
    if (!sanitized.accepted) {
        return std::nullopt;
    }

    std::vector<Terminal_environment_entry> result;
    result.reserve(sanitized.entries.size());
    for (vnm::environment_policy::Environment_entry& entry : sanitized.entries) {
        result.push_back({
            utf8_qstring(entry.name),
            utf8_qstring(entry.value),
        });
    }
    return result;
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
