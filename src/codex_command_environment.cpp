#include "vnm_terminal/app_support/codex_command_environment.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

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

void remove_detection_hints(std::vector<Terminal_environment_entry>& entries)
{
    constexpr std::array<const char*, 15> k_detection_hints{
        "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "GHOSTTY_RESOURCES_DIR",
        "WEZTERM_VERSION", "WEZTERM_EXECUTABLE", "ITERM_SESSION_ID", "ITERM_PROFILE",
        "ITERM_PROFILE_NAME", "TERM_SESSION_ID", "KITTY_WINDOW_ID", "ALACRITTY_SOCKET",
        "KONSOLE_VERSION", "GNOME_TERMINAL_SCREEN", "VTE_VERSION", "WT_SESSION",
    };
#if defined(Q_OS_WIN)
    constexpr Qt::CaseSensitivity k_name_case = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity k_name_case = Qt::CaseSensitive;
#endif
    std::erase_if(entries, [
            &k_detection_hints
        ](const Terminal_environment_entry& entry)
        {
            return std::any_of(k_detection_hints.begin(), k_detection_hints.end(), [
                    &entry
                ](const char* name)
                {
                    return entry.name.compare(QLatin1String(name), k_name_case) == 0;
                });
        });
}

#if defined(Q_OS_WIN)
QString resolve_windows_program(
    const QString& program,
    const Terminal_process_start_request& request,
    const QProcessEnvironment& environment,
    const QStringList& extensions)
{
    const auto resolve_candidate = [
            &extensions
        ](const QString& path)
        {
            if (!QFileInfo(path).suffix().isEmpty()) {
                return QFileInfo(path).isFile() ? QDir::cleanPath(path) : QString();
            }
            for (const QString& extension : extensions) {
                const QString candidate = path + extension;
                if (QFileInfo(candidate).isFile()) {
                    return QDir::cleanPath(candidate);
                }
            }
            return QString();
        };
    if (program.contains(QLatin1Char('/')) ||
        program.contains(QLatin1Char('\\')) ||
        program.contains(QLatin1Char(':')))
    {
        return resolve_candidate(QDir(request.working_directory).absoluteFilePath(program));
    }
    for (QString directory : environment.value(QStringLiteral("PATH")).split(
            QLatin1Char(';'), Qt::KeepEmptyParts))
    {
        if (directory.startsWith(QLatin1Char('"')) && directory.endsWith(QLatin1Char('"'))) {
            directory = directory.mid(1, directory.size() - 2);
        }
        directory = QDir(request.working_directory).absoluteFilePath(directory);
        const QString candidate = resolve_candidate(QDir(directory).filePath(program));
        if (!candidate.isEmpty()) {
            return candidate;
        }
    }
    return {};
}

QString powershell_literal(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString batch_argument(const QString& value)
{
    // Doubled quotes keep cmd metacharacters quoted and reach the native child
    // as literal quotes. A CRT backslash-quote escape would expose those operators.
    QString quoted(QLatin1Char('"'));
    qsizetype backslashes = 0;
    for (const QChar character : value) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        quoted += QString(backslashes * (character == QLatin1Char('"') ? 2 : 1), QLatin1Char('\\'));
        if (character == QLatin1Char('"')) {
            quoted += QLatin1Char('"');
        }
        if (character == QLatin1Char('%')) {
            // Empty substring expansion prevents cmd from expanding a literal %NAME%.
            quoted += QStringLiteral("%%cd:~,%");
        }
        else {
            quoted += character;
        }
        backslashes = 0;
    }
    return quoted + QString(backslashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
}

bool prepare_windows_entry_point(
    const Terminal_process_start_request& request,
    QStringList& command,
    std::optional<QString>& native_arguments,
    QString& error)
{
    QProcessEnvironment environment;
    for (const auto& entry : request.base_environment) {
        environment.insert(entry.name, entry.value);
    }
    QStringList extensions{QStringLiteral(".ps1")};
    extensions += environment.value(
        QStringLiteral("PATHEXT"), QStringLiteral(".COM;.EXE;.BAT;.CMD")).split(
            QLatin1Char(';'), Qt::SkipEmptyParts);
    const QString program = resolve_windows_program(command.front(), request, environment, extensions);
    if (program.isEmpty()) {
        error = QStringLiteral("Could not find the installed Codex command: %1").arg(command.front());
        return false;
    }
    command.front() = program;
    const QString suffix = QFileInfo(program).suffix().toLower();
    const QStringList native_extensions{QStringLiteral(".exe"), QStringLiteral(".com")};
    const QString system_root = environment.value(
        QStringLiteral("SystemRoot"), environment.value(QStringLiteral("WINDIR")));
    if (suffix == QStringLiteral("ps1")) {
        QString powershell = resolve_windows_program(
            QStringLiteral("pwsh.exe"), request, environment, native_extensions);
        if (powershell.isEmpty() && !system_root.isEmpty()) {
            powershell = QDir(system_root).filePath(
                QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
        }
        if (!QFileInfo(powershell).isFile()) {
            error = QStringLiteral("Could not find PowerShell for the installed Codex script.");
            return false;
        }
        QString invocation = QStringLiteral("& ") + powershell_literal(program);
        for (const QString& argument : command.mid(1)) {
            invocation += QLatin1Char(' ') + powershell_literal(argument);
        }
        invocation += QStringLiteral("; exit $LASTEXITCODE");
        command = QStringList{
            powershell, QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"), invocation,
        };
    }
    else
    if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")) {
        QString interpreter = environment.value(QStringLiteral("COMSPEC"));
        if (interpreter.isEmpty() && !system_root.isEmpty()) {
            interpreter = QDir(system_root).filePath(QStringLiteral("System32/cmd.exe"));
        }
        if (!interpreter.isEmpty()) {
            interpreter = resolve_windows_program(interpreter, request, environment, native_extensions);
        }
        if (!QFileInfo(interpreter).isFile()) {
            error = QStringLiteral("Could not find cmd.exe for the installed Codex script.");
            return false;
        }
        QStringList arguments;
        for (const QString& argument : command) {
            if (argument.contains(QLatin1Char('\r')) || argument.contains(QLatin1Char('\n'))) {
                error = QStringLiteral("Windows batch scripts cannot preserve line breaks in Codex arguments.");
                return false;
            }
            arguments.append(batch_argument(argument));
        }
        native_arguments = QStringLiteral("/d /e:on /v:off /s /c \"") +
            arguments.join(QLatin1Char(' ')) + QLatin1Char('"');
        command = QStringList{interpreter};
    }
    return true;
}
#endif

} // namespace

bool prepare_codex_command(
    Terminal_process_start_request& request,
    QString& error,
    std::optional<Codex_invocation> explicit_invocation)
{
    error.clear();
    if (!explicit_invocation && (request.argv.isEmpty() || !names_codex(request.argv.front()))) {
        return true;
    }
    const qsizetype prefix_size = explicit_invocation ? explicit_invocation->argument_prefix_size : 1;
    if (prefix_size < 1 || prefix_size > request.argv.size() ||
        (explicit_invocation && !QFileInfo(request.argv.front()).isAbsolute()))
    {
        error = QStringLiteral("The Codex invocation has an invalid executable prefix.");
        return false;
    }
    if (request.windows_native_arguments) {
        error = QStringLiteral(
            "Direct Codex launch requires structured arguments instead of native Windows arguments.");
        return false;
    }
    QStringList command = request.argv;
    command.insert(prefix_size, QStringLiteral("-c"));
    command.insert(prefix_size + 1, QStringLiteral("shell_environment_policy.set.TERM='xterm-256color'"));
    std::optional<QString> native_arguments;
#if defined(Q_OS_WIN)
    if (!explicit_invocation && !prepare_windows_entry_point(request, command, native_arguments, error)) {
        return false;
    }
#endif
    request.argv                    = std::move(command);
    request.windows_native_arguments = std::move(native_arguments);
    request.terminal_identity        = QStringLiteral("vnm-terminal-sixel");
    remove_detection_hints(request.base_environment);
    if (request.capability_environment) {
        remove_detection_hints(*request.capability_environment);
    }
    return true;
}

} // namespace vnm_terminal::terminal_app
