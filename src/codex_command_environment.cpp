#include "vnm_terminal/app_support/codex_command_environment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QResource>
#include <QStandardPaths>

static bool initialize_codex_resources()
{
    Q_INIT_RESOURCE(codex_commands);
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

QString powershell_array(const QStringList& values)
{
    QStringList literals;
    for (const QString& value : values) {
        literals.append(powershell_literal(value));
    }
    return QStringLiteral("@(") + literals.join(QLatin1Char(',')) + QLatin1Char(')');
}
#endif

} // namespace

bool Codex_command_environment::prepare(
    QStringList& command, const QProcessEnvironment& environment, QString& error,
    std::optional<Codex_invocation> explicit_invocation)
{
    error.clear();
    if (!explicit_invocation && (command.isEmpty() || !names_codex(command.front()))) {
        return true;
    }
    const auto fail_setup = [
            &error
        ](const QString& detail)
        {
            error = detail;
            return false;
        };
    if (explicit_invocation &&
        (explicit_invocation->argument_prefix_size < 1 ||
         explicit_invocation->argument_prefix_size > command.size() ||
         !QFileInfo(command.front()).isAbsolute()))
    {
        return fail_setup(QStringLiteral("The verified Codex invocation has an invalid executable prefix."));
    }
    static const bool resources_initialized = initialize_codex_resources();
    Q_UNUSED(resources_initialized);
#if defined(Q_OS_WIN)
    const QString path = environment.value(QStringLiteral("PATH"));
    QString powershell = QStandardPaths::findExecutable(
        QStringLiteral("pwsh.exe"), path.split(QDir::listSeparator(), Qt::KeepEmptyParts));
    if (powershell.isEmpty()) {
        powershell = QDir(environment.value(QStringLiteral("SystemRoot"))).filePath(
            QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    }
    if (!QFileInfo::exists(powershell)) {
        return fail_setup(QStringLiteral("Could not find Windows PowerShell for the Codex adapter."));
    }
    const QString helper_name = QStringLiteral("launch_codex.ps1");
#else
    Q_UNUSED(environment);
    const QString helper_name = QStringLiteral("launch_codex.sh");
#endif
    if (!m_directory) {
        m_directory.emplace();
    }
    if (!m_directory->isValid()) {
        return fail_setup(QStringLiteral("Could not prepare the Codex launch directory: %1")
            .arg(m_directory->errorString()));
    }
    QFile resource(QStringLiteral(":/vnm_terminal/codex/") + helper_name);
    if (!resource.open(QIODevice::ReadOnly)) {
        return fail_setup(QStringLiteral("Could not read the bundled Codex launcher: %1").arg(helper_name));
    }
    QByteArray helper_content = resource.readAll();
#if defined(Q_OS_WIN)
    // Windows PowerShell needs a BOM for non-ASCII installation paths.
    helper_content.prepend("\xEF\xBB\xBF");
#endif
    QFile output(m_directory->filePath(helper_name));
    if (!output.open(QIODevice::WriteOnly) || output.write(helper_content) != helper_content.size()) {
        return fail_setup(QStringLiteral("Could not write the Codex launch script: %1")
            .arg(output.errorString()));
    }
    output.close();

    const qsizetype prefix_size = explicit_invocation ? explicit_invocation->argument_prefix_size : 1;
#if defined(Q_OS_WIN)
    const QString invocation = QStringLiteral("& ") +
        powershell_literal(output.fileName()) +
        QStringLiteral(" -CommandPrefix ") + powershell_array(command.mid(0, prefix_size)) +
        QStringLiteral(" -Arguments ") + powershell_array(command.mid(prefix_size)) +
        QStringLiteral("; exit $LASTEXITCODE");
    // Script literals preserve argv without -File's quoting losses or command-line size overhead.
    const QByteArray content = QByteArray("\xEF\xBB\xBF") + invocation.toUtf8() + '\n';
    QFile direct_script(m_directory->filePath(QStringLiteral("direct_codex.ps1")));
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
#else
    command = QStringList{
        QStringLiteral("/bin/sh"), output.fileName(), QString::number(prefix_size),
    } + command;
#endif
    return true;
}

} // namespace vnm_terminal::terminal_app
