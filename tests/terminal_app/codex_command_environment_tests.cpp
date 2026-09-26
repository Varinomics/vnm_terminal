#include "codex_command_environment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

const QStringList identity_hints = {
    "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "GHOSTTY_RESOURCES_DIR",
    "WEZTERM_VERSION", "WEZTERM_EXECUTABLE", "ITERM_SESSION_ID", "ITERM_PROFILE",
    "ITERM_PROFILE_NAME", "TERM_SESSION_ID", "KITTY_WINDOW_ID", "ALACRITTY_SOCKET",
    "KONSOLE_VERSION", "GNOME_TERMINAL_SCREEN", "VTE_VERSION", "WT_SESSION",
};
const QStringList multiplexer_hints = {
    "TMUX", "TMUX_PANE", "ZELLIJ", "ZELLIJ_SESSION_NAME", "ZELLIJ_VERSION",
};

void require(bool condition, const QString& message)
{
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

void write_file(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), path);
    file.close();
#if !defined(Q_OS_WIN)
    require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), path);
#endif
}

QString quote_literal(QString value)
{
#if defined(Q_OS_WIN)
    value.replace("'", "''");
#else
    value.replace("'", "'\"'\"'");
#endif
    return "'" + value + "'";
}

int run_fixture(QStringList arguments)
{
#if defined(Q_OS_WIN)
    require(_setmode(_fileno(stdin), _O_BINARY) != -1, "fixture binary stdin");
#endif
    if (!arguments.isEmpty() && arguments.front() == "--fixture") {
        arguments.removeFirst();
    }
    QFile input;
    require(input.open(stdin, QIODevice::ReadOnly), "fixture stdin");
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QJsonObject fields;
    for (const QString& name : identity_hints + multiplexer_hints + QStringList{"TERM", "PATH", "VNM_WRAPPER"}) {
        if (environment.contains(name)) {
            fields.insert(name, environment.value(name));
        }
    }
    const QJsonObject result{
        {"arguments", QJsonArray::fromStringList(arguments)},
        {"environment", fields},
        {"cwd", QDir::currentPath()},
        {"input", QString::fromUtf8(input.readAll())},
    };
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout);
    return 37;
}

void check_launch(
    const QStringList& command,
    const QProcessEnvironment& environment,
    const QString& working_directory,
    const QStringList& arguments,
    const QString& original_path,
    const QString& wrapper,
    const QString& expected_input = QStringLiteral("input from the terminal\n"))
{
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setWorkingDirectory(working_directory);
#if defined(Q_OS_WIN)
    if (QFileInfo(command.front()).fileName().compare("cmd.exe", Qt::CaseInsensitive) == 0) {
        // cmd parses command text, not the CRT argv quoting used by QProcess.
        child.setNativeArguments(command.mid(1).join(' '));
        child.start(command.front(), {});
    }
    else
#endif
    {
        child.start(command.front(), command.mid(1));
    }
    require(child.waitForStarted(), child.errorString());
    child.write("input from the terminal\n");
    child.closeWriteChannel();
    require(child.waitForFinished(15000), "command timed out: " + command.join(' '));
    const QByteArray output = child.readAllStandardOutput();
    const QString diagnostic = QString::fromUtf8(child.readAllStandardError()) + QString::fromUtf8(output);
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 37, diagnostic);
    const QJsonObject result = QJsonDocument::fromJson(output).object();
    const QStringList expected = QStringList{
        "-c", "shell_environment_policy.set.TERM='xterm-256color'",
    } + arguments;
    require(result.value("arguments").toArray() == QJsonArray::fromStringList(expected), diagnostic);
    require(result.value("cwd").toString() == QDir(working_directory).absolutePath(), diagnostic);
    require(result.value("input").toString() == expected_input, diagnostic);
    const QJsonObject fields = result.value("environment").toObject();
    require(fields.value("TERM").toString() == "vnm-terminal-sixel", diagnostic);
    const QString child_path = fields.value("PATH").toString();
#if defined(Q_OS_WIN)
    // PowerShell itself adds PSHOME when it starts, before the adapter runs.
    const QString powershell_directory = QDir::toNativeSeparators(
        QFileInfo(QStandardPaths::findExecutable("pwsh.exe")).absolutePath());
    require(child_path == original_path || child_path == powershell_directory + ';' + original_path, diagnostic);
#else
    require(child_path == original_path, diagnostic);
#endif
    require(fields.value("VNM_WRAPPER").toString() == wrapper, diagnostic);
    for (const QString& name : identity_hints) {
        require(!fields.contains(name), name + ": " + diagnostic);
    }
    for (const QString& name : multiplexer_hints) {
        require(fields.value(name).toString() == "preserved", name + ": " + diagnostic);
    }
}

void run_tests()
{
    const QProcessEnvironment parent = QProcessEnvironment::systemEnvironment();
    QTemporaryDir fixture;
    require(fixture.isValid(), fixture.errorString());
    const QString executable = QCoreApplication::applicationFilePath();
    const QString original_path = fixture.path() + QDir::listSeparator() + parent.value("PATH");
    QProcessEnvironment environment = parent;
    environment.insert("PATH", original_path);
    environment.insert("TERM", "xterm-256color");
    for (const QString& name : identity_hints) {
        environment.insert(name, "inherited-terminal");
    }
    for (const QString& name : multiplexer_hints) {
        environment.insert(name, "preserved");
    }

#if defined(Q_OS_WIN)
    const QString powershell = QStandardPaths::findExecutable("pwsh.exe");
    require(!powershell.isEmpty(), "This Windows compatibility gate requires PowerShell 7.3 or newer.");
    write_file(fixture.filePath("codex"), "#!/bin/sh\nexit 93\n");
    write_file(fixture.filePath("codex.ps1"), (
        "$env:VNM_WRAPPER = 'powershell'\n& " + quote_literal(executable) +
        " --fixture @args\nexit $LASTEXITCODE\n").toUtf8());
    write_file(fixture.filePath("codex.cmd"), (
        "@set VNM_WRAPPER=cmd\r\n@\"" + QDir::toNativeSeparators(executable) +
        "\" --fixture %*\r\n@exit /b %errorlevel%\r\n").toUtf8());
    const QString expected_wrapper = "powershell";
    QStringList shell{powershell, "-NoLogo", "-NoProfile"};
#else
    write_file(fixture.filePath("codex"), (
        "#!/bin/sh\nVNM_WRAPPER=posix\nexport VNM_WRAPPER\nexec " + quote_literal(executable) +
        " --fixture \"$@\"\n").toUtf8());
    const QString expected_wrapper = "posix";
    QStringList shell{"/bin/sh"};
#endif
    const QStringList arguments{"", "with spaces", "embedded\"quote", "--flag", "unicode-\u03bb"};
    QStringList direct = QStringList{"codex"} + arguments;
    QString error;
    QString private_path;
    {
        vnm_terminal::terminal_app::Codex_command_environment commands;
        require(commands.prepare(direct, environment, error), error);
        private_path = environment.value("PATH").section(QDir::listSeparator(), 0, 0);
        require(QDir(private_path).exists(), "private command directory missing");
        require(environment.value("TERM") == "xterm-256color", "ordinary terminal TERM changed");
        require(environment.value("TERM_PROGRAM") == "inherited-terminal", "ordinary terminal hint changed");
        require(QProcessEnvironment::systemEnvironment() == parent, "parent environment changed");
        check_launch(direct, environment, fixture.path(), arguments, original_path, expected_wrapper);

        const QProcessEnvironment shell_environment = environment;
        QStringList shell_command = shell;
#if defined(Q_OS_WIN)
        QString invocation = "$PSNativeCommandUseErrorActionPreference = $true; codex";
        for (const QString& argument : arguments) {
            invocation += ' ' + quote_literal(argument);
        }
        invocation += "; $code = $LASTEXITCODE; "
            "if ($env:TERM -ne 'xterm-256color' -or $env:TERM_PROGRAM -ne 'inherited-terminal') { exit 92 }; "
            "exit $code";
        shell_command += QStringList{"-Command", invocation};
#else
        shell_command += QStringList{"-c", "codex \"$@\"", "fixture"} + arguments;
#endif
        check_launch(shell_command, shell_environment, fixture.path(), arguments, original_path, expected_wrapper);

        QProcessEnvironment explicit_environment = environment;
        explicit_environment.insert("PATH", original_path);
#if defined(Q_OS_WIN)
        QStringList explicit_command = QStringList{fixture.filePath("codex.ps1")} + arguments;
#else
        QStringList explicit_command = QStringList{fixture.filePath("codex")} + arguments;
#endif
        vnm_terminal::terminal_app::Codex_command_environment explicit_commands;
        require(explicit_commands.prepare(explicit_command, explicit_environment, error), error);
        check_launch(explicit_command, explicit_environment, fixture.path(), arguments, original_path, expected_wrapper);

#if defined(Q_OS_WIN)
        // The default cmd shell must retain its own PATHEXT wrapper choice.
        const QStringList cmd_arguments{"with spaces", "--flag"};
        const QStringList cmd{
            qEnvironmentVariable("COMSPEC"), "/d", "/c", "codex \"with spaces\" --flag",
        };
        QTemporaryDir working_directory;
        require(working_directory.isValid(), working_directory.errorString());
        check_launch(cmd, environment, working_directory.path(), cmd_arguments, original_path, "cmd");

        // npm's PowerShell shim forwards object pipelines; raw process input must stay raw.
        const QString native_command = "& " + quote_literal(executable) + " --fixture @args";
        write_file(fixture.filePath("codex.ps1"), (
            "$env:VNM_WRAPPER = 'powershell'\nif ($MyInvocation.ExpectingInput) { $input | " +
            native_command + " } else { " + native_command + " }\nexit $LASTEXITCODE\n").toUtf8());
        check_launch(direct, environment, fixture.path(), arguments, original_path, "powershell");
        QString pipeline_invocation = "'input from the terminal' | codex";
        for (const QString& argument : arguments) {
            pipeline_invocation += ' ' + quote_literal(argument);
        }
        pipeline_invocation += "; exit $LASTEXITCODE";
        check_launch(shell + QStringList{"-Command", pipeline_invocation}, environment,
            fixture.path(), arguments, original_path, "powershell", "input from the terminal\r\n");

        QProcessEnvironment unavailable_environment = parent;
        unavailable_environment.insert("PATH", fixture.path());
        const QProcessEnvironment unmodified = unavailable_environment;
        QStringList ordinary{qEnvironmentVariable("COMSPEC")};
        vnm_terminal::terminal_app::Codex_command_environment unavailable;
        require(unavailable.prepare(ordinary, unavailable_environment, error), error);
        require(unavailable_environment == unmodified, "missing PowerShell changed ordinary shell environment");
        QStringList unsupported{"codex"};
        require(!unavailable.prepare(unsupported, unavailable_environment, error), "missing prerequisite accepted");
#endif
        // A failed optional shim write must not prevent an unrelated shell from starting.
#if defined(Q_OS_WIN)
        const QString blocked_file = QDir(private_path).filePath("codex.ps1");
#else
        const QString blocked_file = QDir(private_path).filePath("codex");
#endif
        require(QFile::remove(blocked_file) && QDir().mkdir(blocked_file), "could not inject shim write failure");
        QProcessEnvironment failed_environment = environment;
        failed_environment.insert("PATH", original_path);
        const QProcessEnvironment before_failure = failed_environment;
        QStringList ordinary_command = shell;
        require(commands.prepare(ordinary_command, failed_environment, error), "optional setup blocked shell");
        require(ordinary_command == shell && failed_environment == before_failure, "failed setup changed launch");
        QStringList failed_codex{"codex"};
        require(!commands.prepare(failed_codex, failed_environment, error) && !error.isEmpty(),
            "direct Codex setup failure was hidden");
    }
    require(!QDir(private_path).exists(), "terminal-local command directory survived its owner");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        const QStringList arguments = application.arguments().mid(1);
        if (arguments.value(0) == "--fixture") {
            return run_fixture(arguments);
        }
        run_tests();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
