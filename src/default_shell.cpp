#include "vnm_terminal/default_shell.h"

#include <QString>
#include <QtGlobal>

namespace vnm_terminal {

namespace {

QString environment_or_default(const char* name, const QString& fallback)
{
    const QString value = QString::fromLocal8Bit(qgetenv(name));
    return value.trimmed().isEmpty() ? fallback : value;
}

} // namespace

QStringList default_shell_argv()
{
#if defined(_WIN32)
    return {environment_or_default("COMSPEC", QStringLiteral("cmd.exe"))};
#elif defined(__linux__) || defined(__APPLE__)
    return {environment_or_default("SHELL", QStringLiteral("/bin/sh"))};
#else
    return {};
#endif
}

} // namespace vnm_terminal
