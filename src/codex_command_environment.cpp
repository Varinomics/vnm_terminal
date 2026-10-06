#include "vnm_terminal/app_support/codex_command_environment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QResource>
#include <QStandardPaths>

static bool initialize_codex_resources()
{
    Q_INIT_RESOURCE(codex_commands);
#if defined(Q_OS_WIN)
    Q_INIT_RESOURCE(codex_launcher);
#endif
    return true;
}

namespace vnm_terminal::terminal_app {
namespace {

bool names_codex(const QString& program)
{
    QString name = QFileInfo(program).fileName();
#if defined(Q_OS_WIN)
    name = name.toLower();
    for (const QString& extension : {
        QStringLiteral(".exe"), QStringLiteral(".cmd"), QStringLiteral(".bat"), QStringLiteral(".ps1")})
    {
        if (name.endsWith(extension)) {
            name.chop(extension.size());
            break;
        }
    }
#endif
    return name == QStringLiteral("codex") || name == QStringLiteral("codex-pet");
}

#if defined(Q_OS_WIN)
QString powershell_literal(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString native_argument_literal(const QString& value)
{
    QString quoted = QStringLiteral("\"");
    qsizetype backslashes = 0;
    for (const QChar character : value) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        const qsizetype escaped_count = character == QLatin1Char('"')
            ? 2 * backslashes + 1 : backslashes;
        quoted += QString(escaped_count, QLatin1Char('\\'));
        quoted += character;
        backslashes = 0;
    }
    quoted += QString(2 * backslashes, QLatin1Char('\\')) + QLatin1Char('"');
    return quoted;
}
#endif

} // namespace

bool Codex_command_environment::prepare(
    QStringList& command, QProcessEnvironment& environment, QString& error,
    std::optional<Codex_invocation> explicit_invocation)
{
    static const bool resources_initialized = initialize_codex_resources();
    Q_UNUSED(resources_initialized);
    const bool direct_codex = explicit_invocation.has_value() ||
        (!command.isEmpty() && names_codex(command.front()));
    const auto fail_setup = [&](const QString& detail) {
        error = direct_codex ? detail : QString();
        return !direct_codex;
    };
    if (explicit_invocation &&
        (explicit_invocation->argument_prefix_size < 1 ||
         explicit_invocation->argument_prefix_size > command.size() ||
         !QFileInfo(command.front()).isAbsolute()))
    {
        return fail_setup(QStringLiteral("The verified Codex invocation has an invalid executable prefix."));
    }
    const QString path = environment.value(QStringLiteral("PATH"));
#if defined(Q_OS_WIN)
    QString powershell;
    if (!explicit_invocation) {
        powershell = QStandardPaths::findExecutable(
            QStringLiteral("pwsh.exe"), path.split(QDir::listSeparator(), Qt::KeepEmptyParts));
        if (powershell.isEmpty()) {
            powershell = QDir(environment.value(QStringLiteral("SystemRoot"))).filePath(
                QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
        }
        if (!QFileInfo::exists(powershell)) {
            return fail_setup(QStringLiteral("Could not find Windows PowerShell for the Codex adapter."));
        }
    }
    const QStringList files = {
        QStringLiteral("codex.ps1"),
        QStringLiteral("codex.exe"),
        QStringLiteral("codex-pet.ps1"),
        QStringLiteral("codex-pet.exe"),
        QStringLiteral("launch_codex.ps1"),
        QStringLiteral("codex_launcher.exe"),
    };
#else
    const QStringList files = {
        QStringLiteral("codex"), QStringLiteral("codex-pet"), QStringLiteral("launch_codex.sh")};
#endif
    if (!m_directory.isValid()) {
        return fail_setup(QStringLiteral("Could not prepare terminal-local Codex commands: %1")
            .arg(m_directory.errorString()));
    }
    for (const QString& name : files) {
        QString source_name = name;
        source_name.replace(QStringLiteral("codex-pet"), QStringLiteral("codex"));
#if defined(Q_OS_WIN)
        if (source_name == QStringLiteral("codex.exe")) {
            source_name = QStringLiteral("codex_launcher.exe");
        }
#endif
        QFile resource(QStringLiteral(":/vnm_terminal/codex/") + source_name);
        if (!resource.open(QIODevice::ReadOnly)) {
            return fail_setup(QStringLiteral("Could not read the bundled Codex command: %1").arg(name));
        }
        QByteArray content = resource.readAll();
        if (source_name == QStringLiteral("codex") || source_name == QStringLiteral("codex.ps1"))
        {
            content.replace("@CODEX_COMMAND@", name.startsWith(QStringLiteral("codex-pet")) ? "codex-pet" : "codex");
        }
#if defined(Q_OS_WIN)
        if (name.endsWith(QStringLiteral(".ps1"))) {
            // Windows PowerShell needs a BOM for non-ASCII installation paths.
            content.prepend("\xEF\xBB\xBF");
        }
#endif
        QFile output(m_directory.filePath(name));
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size()) {
            return fail_setup(QStringLiteral("Could not write the terminal-local Codex command: %1")
                .arg(output.errorString()));
        }
        output.close();
#if !defined(Q_OS_WIN)
        if (!output.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            return fail_setup(QStringLiteral("Could not make the terminal-local Codex command executable."));
        }
#endif
    }
    if (direct_codex) {
#if defined(Q_OS_WIN)
        if (explicit_invocation) {
            const qsizetype prefix_size = explicit_invocation->argument_prefix_size;
            const QStringList arguments = command.mid(1, prefix_size - 1) + QStringList{
                QStringLiteral("-c"), QStringLiteral("shell_environment_policy.set.TERM='xterm-256color'"),
            } + command.mid(prefix_size);
            QStringList quoted_arguments;
            for (const QString& argument : arguments) {
                quoted_arguments.append(native_argument_literal(argument));
            }
            const QString native_arguments = quoted_arguments.join(QLatin1Char(' '));
            const QByteArray content(
                reinterpret_cast<const char*>(native_arguments.utf16()),
                native_arguments.size() * sizeof(char16_t));
            QFile argument_file(m_directory.filePath(QStringLiteral("direct_codex_arguments")));
            if (!argument_file.open(QIODevice::WriteOnly) || argument_file.write(content) != content.size()) {
                return fail_setup(QStringLiteral("Could not prepare the verified Codex arguments: %1")
                    .arg(argument_file.errorString()));
            }
            argument_file.close();
            command = QStringList{
                m_directory.filePath(QStringLiteral("codex_launcher.exe")),
                QStringLiteral("--arguments-file"), command.front(), argument_file.fileName(),
            };
        }
        else {
            QString invocation = QStringLiteral("& ") + powershell_literal(
                m_directory.filePath(QStringLiteral("launch_codex.ps1")));
            for (const QString& argument : command) {
                invocation += QLatin1Char(' ') + powershell_literal(argument);
            }
            invocation += QStringLiteral("; exit $LASTEXITCODE");
            // Script literals preserve argv without -File's quoting losses or encoded command-line size overhead.
            const QByteArray content = QByteArray("\xEF\xBB\xBF") + invocation.toUtf8() + '\n';
            QFile direct_script(m_directory.filePath(QStringLiteral("direct_codex.ps1")));
            if (!direct_script.open(QIODevice::WriteOnly) || direct_script.write(content) != content.size()) {
                return fail_setup(QStringLiteral("Could not prepare the direct Codex invocation: %1")
                    .arg(direct_script.errorString()));
            }
            direct_script.close();
            command = QStringList{
                powershell,
                QStringLiteral("-NoLogo"),
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-File"),
                direct_script.fileName(),
            };
        }
#else
        command = QStringList{
            QStringLiteral("/bin/sh"),
            m_directory.filePath(QStringLiteral("launch_codex.sh")),
            QString::number(explicit_invocation ? explicit_invocation->argument_prefix_size : 1),
        } + command;
#endif
    }
    environment.insert(QStringLiteral("PATH"), m_directory.path() + QDir::listSeparator() + path);
    return true;
}

} // namespace vnm_terminal::terminal_app
