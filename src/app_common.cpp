#include "app_common.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include <iostream>

namespace vnm_terminal::terminal_app {

bool validate_output_path(
    const QString& option_name,
    const QString& path,
    QString*       out_absolute_path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("%1 requires a non-empty path").arg(option_name);
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("%1 parent directory does not exist: %2")
            .arg(option_name, parent_dir.absolutePath());
        return false;
    }
    if (file_info.exists() && file_info.isDir()) {
        *out_error = QStringLiteral("%1 points to a directory: %2")
            .arg(option_name, file_info.absoluteFilePath());
        return false;
    }

    *out_absolute_path = file_info.absoluteFilePath();
    return true;
}

void print_error(const QString& message)
{
    const QByteArray bytes = message.toUtf8();
    std::cerr << "vnm_terminal: " << bytes.constData() << '\n';
}

} // namespace vnm_terminal::terminal_app
