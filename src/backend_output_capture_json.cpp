#include "vnm_terminal/app_support/backend_output_capture_json.h"

#include <QJsonValue>

#include <limits>

namespace vnm_terminal::terminal_app {

QJsonObject backend_output_capture_config_to_json(
    const Backend_output_capture_config& config)
{
    return {
        {QStringLiteral("base_path"), config.base_path},
        {QStringLiteral("max_bytes"),
         QString::number(static_cast<qulonglong>(config.max_bytes))},
    };
}

std::optional<Backend_output_capture_config> backend_output_capture_config_from_json(
    const QJsonObject& object)
{
    const QJsonValue base_path = object.value(QStringLiteral("base_path"));
    const QJsonValue max_bytes = object.value(QStringLiteral("max_bytes"));
    bool max_bytes_ok = false;
    const qulonglong parsed_max_bytes = max_bytes.toString().toULongLong(&max_bytes_ok);
    if (object.size() != 2 || !base_path.isString() ||
        base_path.toString().isEmpty() ||
        base_path.toString().contains(QChar(u'\0')) ||
        !max_bytes.isString() || !max_bytes_ok || parsed_max_bytes == 0 ||
        max_bytes.toString() != QString::number(parsed_max_bytes) ||
        parsed_max_bytes > std::numeric_limits<std::size_t>::max())
    {
        return std::nullopt;
    }
    return Backend_output_capture_config{
        base_path.toString(),
        static_cast<std::size_t>(parsed_max_bytes),
    };
}

} // namespace vnm_terminal::terminal_app
