#include "vnm_terminal/app_support/codex_command_environment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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

using vnm_terminal::terminal_app::Codex_command_environment;
using vnm_terminal::terminal_app::Codex_invocation;

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

bool same_existing_directory(const QString& actual, const QString& expected)
{
    const QString canonical_expected = QDir(expected).canonicalPath();
#if defined(Q_OS_WIN)
    const Qt::CaseSensitivity path_case = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity path_case = Qt::CaseSensitive;
#endif
    return !canonical_expected.isEmpty() &&
        QFileInfo(actual).isDir() && QFileInfo(expected).isDir() &&
        QDir(actual).canonicalPath().compare(canonical_expected, path_case) == 0;
}

int run_fixture(QStringList arguments, bool has_entry_point = false)
{
#if defined(Q_OS_WIN)
    require(_setmode(_fileno(stdin), _O_BINARY) != -1, "fixture binary stdin");
#endif
    QString entry_point;
    if (has_entry_point) {
        require(!arguments.isEmpty(), "fixture entry point missing");
        entry_point = arguments.takeFirst();
    }
    else
    if (!arguments.isEmpty() && arguments.front() == "--fixture") {
        arguments.removeFirst();
    }
    else
    if (!arguments.isEmpty() && arguments.front() == "--fixture-encoded") {
        require(arguments.size() == 2, "fixture encoded argument count");
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromBase64(arguments.at(1).toLatin1()));
        require(document.isArray(), "fixture encoded arguments must be an array");
        arguments.clear();
        for (const QJsonValue& argument : document.array()) {
            require(argument.isString(), "fixture argument must be a string");
            arguments.append(argument.toString());
        }
    }
    QFile input;
    require(input.open(stdin, QIODevice::ReadOnly), "fixture stdin");
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QJsonObject fields;
    for (const QString& name : identity_hints + multiplexer_hints +
        QStringList{"TERM", "PATH", "VNM_WRAPPER", "VNM_POWERSHELL_HOST"})
    {
        if (environment.contains(name)) {
            fields.insert(name, environment.value(name));
        }
    }
    fields.insert("VNM_WRAPPER", environment.value("VNM_WRAPPER", "native"));
    const QJsonObject result{
        {"arguments", QJsonArray::fromStringList(arguments)},
        {"entry_point", entry_point},
        {"environment", fields},
        {"cwd", QDir::currentPath()},
        {"input", QString::fromUtf8(input.readAll())},
    };
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout);
    return 37;
}

QJsonObject launch_fixture(
    const QStringList& command,
    const QProcessEnvironment& environment,
    const QString& working_directory)
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
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 37,
        QStringLiteral("fixture exit status %1, code %2: ").arg(child.exitStatus()).arg(child.exitCode()) + diagnostic);
    const QJsonDocument document = QJsonDocument::fromJson(output);
    require(document.isObject(), diagnostic);
    return document.object();
}

void check_launch(
    const QStringList& command,
    const QProcessEnvironment& environment,
    const QString& working_directory,
    const QStringList& arguments,
    const QString& original_path,
    const QString& wrapper,
    const QString& expected_input = QStringLiteral("input from the terminal\n"),
    const QString& expected_host = QString(),
    const QString& expected_entry_point = QString())
{
    const QJsonObject result = launch_fixture(command, environment, working_directory);
    const QString diagnostic = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    const QStringList expected = QStringList{
        "-c", "shell_environment_policy.set.TERM='xterm-256color'",
    } + arguments;
    require(result.value("arguments").toArray() == QJsonArray::fromStringList(expected), "fixture argv: " + diagnostic);
    require(result.value("entry_point").toString() == expected_entry_point, "fixture entry point: " + diagnostic);
    require(same_existing_directory(result.value("cwd").toString(), working_directory), "fixture cwd: " + diagnostic);
    require(result.value("input").toString() == expected_input, "fixture stdin: " + diagnostic);
    const QJsonObject fields = result.value("environment").toObject();
    require(fields.value("TERM").toString() == "vnm-terminal-sixel", "fixture TERM: " + diagnostic);
    const QString child_path = fields.value("PATH").toString();
#if defined(Q_OS_WIN)
    // PowerShell itself adds PSHOME when it starts, before the adapter runs.
    const QString host_directory = child_path.section(';', 0, 0);
    const bool host_prefix =
        (QFileInfo::exists(QDir(host_directory).filePath("powershell.exe")) ||
         QFileInfo::exists(QDir(host_directory).filePath("pwsh.exe"))) &&
        child_path.section(';', 1) == original_path;
    require(child_path == original_path || host_prefix, "fixture PATH: " + diagnostic);
#else
    require(child_path == original_path, "fixture PATH: " + diagnostic);
#endif
    require(fields.value("VNM_WRAPPER").toString() == wrapper, "fixture wrapper: " + diagnostic);
    if (!expected_host.isEmpty()) {
        require(same_existing_directory(fields.value("VNM_POWERSHELL_HOST").toString(), expected_host),
            "fixture PowerShell host: " + diagnostic);
    }
    for (const QString& name : identity_hints) {
        require(!fields.contains(name), name + ": " + diagnostic);
    }
    for (const QString& name : multiplexer_hints) {
        require(fields.value(name).toString() == "preserved", name + ": " + diagnostic);
    }
}

#if defined(Q_OS_WIN)
QStringList original_cmd_arguments(
    const QStringList& command, QProcessEnvironment environment,
    const QString& working_directory, const QString& original_path)
{
    environment.insert("PATH", original_path);
    const QJsonObject original = launch_fixture(command, environment, working_directory);
    QStringList arguments;
    for (const QJsonValue& argument : original.value("arguments").toArray()) {
        arguments.append(argument.toString());
    }
    return arguments;
}

void write_powershell_fixture(const QString& path, const QString& executable)
{
    // Observe argv at the original script boundary, before the host's native argument forwarding.
    const QString native_command = "& " + quote_literal(executable) + " --fixture-encoded $payload";
    const QString script =
        "$env:VNM_WRAPPER = 'powershell'\n"
        "$env:VNM_POWERSHELL_HOST = $PSHOME\n"
        "$arguments_json = ConvertTo-Json -Compress -InputObject @($args)\n"
        "$payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($arguments_json))\n"
        "if ($MyInvocation.ExpectingInput) { $input | " + native_command +
        " } else { " + native_command + " }\nexit $LASTEXITCODE\n";
    write_file(path, QByteArray("\xEF\xBB\xBF") + script.toUtf8());
}

QString powershell_invocation(const QString& command, const QStringList& arguments, bool pipeline = false)
{
    QString invocation = "$PSNativeCommandUseErrorActionPreference = $true; ";
    if (pipeline) {
        invocation += "'input from the terminal' | ";
    }
    invocation += "& " + quote_literal(command);
    for (const QString& argument : arguments) {
        invocation += ' ' + quote_literal(argument);
    }
    invocation += "; $code = $LASTEXITCODE; "
        "if ($env:TERM -ne 'xterm-256color' -or $env:TERM_PROGRAM -ne 'inherited-terminal') { exit 92 }; "
        "exit $code";
    return invocation;
}

void check_powershell_launches(
    const QString& host, const QString& command,
    const QProcessEnvironment& environment, const QString& working_directory,
    const QStringList& arguments, const QString& original_path,
    const QString& wrapper)
{
    const QString host_directory = wrapper == "powershell" ? QFileInfo(host).absolutePath() : QString();
    const QStringList shell{
        host, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text", "-Command"};
    check_launch(shell + QStringList{powershell_invocation(command, arguments)}, environment,
        working_directory, arguments, original_path, wrapper, "input from the terminal\n", host_directory);
    check_launch(shell + QStringList{powershell_invocation(command, arguments, true)}, environment,
        working_directory, arguments, original_path, wrapper, "input from the terminal\r\n", host_directory);
}
#endif

void check_verified_invocations(
    const QString& executable,
    const QString& fixture_directory,
    const QProcessEnvironment& environment,
    const QString& original_path,
    const QStringList& arguments)
{
#if defined(Q_OS_WIN)
    const QString executable_extension = QStringLiteral(".exe");
#else
    const QString executable_extension;
#endif
    const QString node = QDir(fixture_directory).filePath("node" + executable_extension);
    const QString native_codex = QDir(fixture_directory).filePath("verified-codex" + executable_extension);
    require(QFile::copy(executable, node), "copy interpreter fixture");
    require(QFile::copy(executable, native_codex), "copy verified native fixture");
    const QString entry_point = QDir(fixture_directory).filePath("npm modules/@openai/codex/bin/codex.js");
    require(QDir().mkpath(QFileInfo(entry_point).absolutePath()), "create npm entry point directory");
    write_file(entry_point, "// The interpreter fixture records this entry point without evaluating it.\n");
    const QStringList codex_arguments = QStringList{
        "-c", "hooks.session_start='logonomic hook'",
        "-c", "shell_environment_policy.set.TERM='user-override'",
        "resume", "019bc485-d9e8-76ae-9fcb-bf375cef3501",
    } + arguments;
    const QStringList node_command = QStringList{node, entry_point} + codex_arguments;
    QString error;
    {
        Codex_command_environment commands;
        QProcessEnvironment launch_environment = environment;
        launch_environment.insert("PATH", original_path);
        for (const QStringList& cli_arguments : {
            codex_arguments, QStringList{}, QStringList{"--prompt", QString(16000, QLatin1Char('p'))}})
        {
            launch_environment.insert("PATH", original_path);
            QStringList command = QStringList{node, entry_point} + cli_arguments;
            require(commands.prepare(command, launch_environment, error, Codex_invocation{2}), error);
            check_launch(command, launch_environment, fixture_directory, cli_arguments, original_path, "native",
                "input from the terminal\n", QString(), entry_point);
        }
    }
    {
        Codex_command_environment commands;
        QProcessEnvironment launch_environment = environment;
        launch_environment.insert("PATH", original_path);
        QStringList command = QStringList{native_codex} + codex_arguments;
        require(commands.prepare(command, launch_environment, error, Codex_invocation{1}), error);
        check_launch(command, launch_environment, fixture_directory, codex_arguments, original_path, "native");
    }
    {
        Codex_command_environment commands;
        QProcessEnvironment launch_environment = environment;
        launch_environment.insert("PATH", original_path);
        QStringList command = node_command;
        require(commands.prepare(command, launch_environment, error), error);
        require(command == node_command, "ordinary Node command was adapted as Codex");
        const QJsonObject result = launch_fixture(command, launch_environment, fixture_directory);
        const QJsonObject fields = result.value("environment").toObject();
        require(result.value("arguments").toArray() == QJsonArray::fromStringList(codex_arguments),
            "ordinary Node arguments changed");
        require(result.value("entry_point").toString() == entry_point, "ordinary Node entry point changed");
        require(fields.value("TERM").toString() == "xterm-256color", "ordinary Node TERM changed");
        require(fields.value("TERM_PROGRAM").toString() == "inherited-terminal", "ordinary Node hint changed");
    }
    for (const qsizetype prefix_size : {qsizetype(-1), qsizetype(0), node_command.size() + 1}) {
        Codex_command_environment commands;
        QProcessEnvironment launch_environment = environment;
        QStringList command = node_command;
        require(!commands.prepare(command, launch_environment, error, Codex_invocation{prefix_size}) &&
            !error.isEmpty(), "invalid verified prefix was accepted");
        require(command == node_command && launch_environment == environment,
            "invalid verified prefix changed the launch");
    }
    {
        Codex_command_environment commands;
        QProcessEnvironment launch_environment = environment;
        QStringList command{"node", entry_point};
        require(!commands.prepare(command, launch_environment, error, Codex_invocation{2}) &&
            !error.isEmpty(), "unverified relative executable was accepted");
    }
}

void run_tests()
{
    const QProcessEnvironment parent = QProcessEnvironment::systemEnvironment();
    QTemporaryDir fixture(QDir::temp().filePath("vnm codex \u03bb-XXXXXX"));
    require(fixture.isValid(), fixture.errorString());
    QTemporaryDir other_directory;
    require(other_directory.isValid(), other_directory.errorString());
    require(same_existing_directory(fixture.path(), fixture.path()), "existing cwd identity rejected");
#if defined(Q_OS_WIN)
    require(same_existing_directory(fixture.path().toUpper(), fixture.path()), "Windows cwd casing rejected");
#endif
    require(!same_existing_directory(fixture.path(), other_directory.path()), "different cwd accepted");
    const QString missing_directory = fixture.filePath("missing");
    require(!same_existing_directory(missing_directory, missing_directory), "missing cwd accepted");
    const QString executable = QCoreApplication::applicationFilePath();
    QString system_path = parent.value("PATH");
#if defined(Q_OS_WIN)
    const QString windows_powershell = QDir(parent.value("SystemRoot")).filePath(
        QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    require(QFileInfo::exists(windows_powershell), "stock Windows PowerShell is missing");
    const QString optional_powershell = QStandardPaths::findExecutable("pwsh.exe");
    QStringList stock_path;
    for (const QString& entry : system_path.split(';', Qt::KeepEmptyParts)) {
        if (!QFileInfo::exists(QDir(entry).filePath("pwsh.exe"))) {
            stock_path.append(entry);
        }
    }
    system_path = stock_path.join(';');
    require(QStandardPaths::findExecutable("pwsh.exe", stock_path).isEmpty(), "stock PATH contains pwsh");
#endif
    const QString original_path = fixture.path() + QDir::listSeparator() + system_path;
    QProcessEnvironment environment = parent;
    environment.insert("PATH", original_path);
    environment.insert("TERM", "xterm-256color");
    environment.remove("VNM_WRAPPER");
    environment.remove("VNM_POWERSHELL_HOST");
    for (const QString& name : identity_hints) {
        environment.insert(name, "inherited-terminal");
    }
    for (const QString& name : multiplexer_hints) {
        environment.insert(name, "preserved");
    }

#if defined(Q_OS_WIN)
    environment.insert("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        write_file(fixture.filePath(name), "#!/bin/sh\nexit 93\n");
        write_powershell_fixture(fixture.filePath(name + ".ps1"), executable);
        write_file(fixture.filePath(name + ".cmd"), (
            "@set VNM_WRAPPER=cmd\r\n@\"" + QDir::toNativeSeparators(executable) +
            "\" --fixture %*\r\n@exit /b %errorlevel%\r\n").toUtf8());
    }
    const QString expected_wrapper = "powershell";
    const QStringList shell{windows_powershell, "-NoLogo", "-NoProfile"};
#else
    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        write_file(fixture.filePath(name), (
            "#!/bin/sh\nVNM_WRAPPER=posix\nexport VNM_WRAPPER\nexec " + quote_literal(executable) +
            " --fixture \"$@\"\n").toUtf8());
    }
    const QString expected_wrapper = "posix";
    QStringList shell{"/bin/sh"};
#endif
    const QStringList arguments{
        "", "with spaces", "embedded\"quote", "single'quote", "--flag", "unicode-\u03bb",
        "C:\\working files\\", "backslashes\\\\before\\\"quote", "percent%value",
        "amp&ersand", "pipe|value", "caret^value", "bang!value", "(parentheses)",
        "dollar$value", "line\nbreak"};
    check_verified_invocations(executable, fixture.path(), environment, original_path, arguments);
    QStringList direct = QStringList{"codex"} + arguments;
    QString error;
    QString private_path;
    {
        Codex_command_environment commands;
        require(commands.prepare(direct, environment, error), error);
        private_path = environment.value("PATH").section(QDir::listSeparator(), 0, 0);
        require(QDir(private_path).exists(), "private command directory missing");
        require(environment.value("TERM") == "xterm-256color", "ordinary terminal TERM changed");
        require(environment.value("TERM_PROGRAM") == "inherited-terminal", "ordinary terminal hint changed");
        require(QProcessEnvironment::systemEnvironment() == parent, "parent environment changed");
        check_launch(direct, environment, fixture.path(), arguments, original_path, expected_wrapper);
#if !defined(Q_OS_WIN)
        const QString linked_directory = other_directory.filePath("linked-cwd");
        require(QFile::link(fixture.path(), linked_directory), "could not create cwd symlink");
        require(same_existing_directory(linked_directory, fixture.path()), "cwd symlink identity rejected");
        // POSIX getcwd reports the physical directory, not the spelling passed to chdir.
        check_launch(direct, environment, linked_directory, arguments, original_path, expected_wrapper);
#endif

        const QProcessEnvironment shell_environment = environment;
#if defined(Q_OS_WIN)
        check_powershell_launches(windows_powershell, "codex", shell_environment,
            fixture.path(), arguments, original_path, expected_wrapper);
        if (!optional_powershell.isEmpty()) {
            check_powershell_launches(optional_powershell, "codex", shell_environment,
                fixture.path(), arguments, original_path, expected_wrapper);
        }
#else
        QStringList shell_command = shell;
        shell_command += QStringList{"-c", "codex \"$@\"", "fixture"} + arguments;
        check_launch(shell_command, shell_environment, fixture.path(), arguments, original_path, expected_wrapper);
#endif

        for (const QString& name : QStringList{"codex", "codex-pet"}) {
            QProcessEnvironment explicit_environment = environment;
            explicit_environment.insert("PATH", original_path);
#if defined(Q_OS_WIN)
            QStringList explicit_command = QStringList{fixture.filePath(name + ".ps1")} + arguments;
#else
            QStringList explicit_command = QStringList{fixture.filePath(name)} + arguments;
#endif
            Codex_command_environment explicit_commands;
            require(explicit_commands.prepare(explicit_command, explicit_environment, error), error);
            check_launch(explicit_command, explicit_environment,
                fixture.path(), arguments, original_path, expected_wrapper);
        }

#if defined(Q_OS_WIN)
        // The default cmd shell must retain its own PATHEXT wrapper choice.
        const QString cmd_tail =
            " \"\" \"with spaces\" --flag \"unicode-\u03bb\" \"C:\\working files\\\\\" "
            "\"amp&ersand\" \"pipe|value\" \"caret^value\" \"bang!value\" \"(parentheses)\" "
            "\"percent%value\" \"dollar$value\" percent%value dollar$value unquoted!value unquoted^^caret";
        for (const QString& name : QStringList{"codex", "codex-pet"}) {
            const QStringList cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + cmd_tail};
            const QStringList cmd_arguments = original_cmd_arguments(
                cmd, environment, other_directory.path(), original_path);
            check_launch(cmd,
                environment, other_directory.path(), cmd_arguments, original_path, "cmd");

            QProcessEnvironment explicit_cmd_environment = environment;
            explicit_cmd_environment.insert("PATH", original_path);
            const QStringList direct_cmd_arguments{"", "with spaces", "--flag", "amp&ersand", "unicode-\u03bb"};
            QStringList explicit_cmd = QStringList{fixture.filePath(name + ".cmd")} + direct_cmd_arguments;
            require(commands.prepare(explicit_cmd, explicit_cmd_environment, error), error);
            check_launch(explicit_cmd, explicit_cmd_environment,
                other_directory.path(), direct_cmd_arguments, original_path, "cmd");
        }
#endif

        QProcessEnvironment pet_environment = environment;
        pet_environment.insert("PATH", original_path);
        QStringList pet_command = QStringList{"codex-pet"} + arguments;
        require(commands.prepare(pet_command, pet_environment, error), error);
        check_launch(pet_command, pet_environment, fixture.path(), arguments, original_path, expected_wrapper);
#if defined(Q_OS_WIN)
        check_powershell_launches(windows_powershell, "codex-pet", pet_environment,
            fixture.path(), arguments, original_path, expected_wrapper);
        if (!optional_powershell.isEmpty()) {
            check_powershell_launches(optional_powershell, "codex-pet", pet_environment,
                fixture.path(), arguments, original_path, expected_wrapper);
        }

        // Native executables must receive every argument directly, without the fixture's encoded transport.
        for (const QString& name : QStringList{"codex", "codex-pet"}) {
            require(QFile::remove(fixture.filePath(name + ".ps1")), "remove PowerShell fixture");
            require(QFile::copy(executable, fixture.filePath(name + ".exe")), "copy native fixture");
            QProcessEnvironment native_environment = environment;
            native_environment.insert("PATH", original_path);
            QStringList native_command = QStringList{fixture.filePath(name + ".exe")} + arguments;
            require(commands.prepare(native_command, native_environment, error), error);
            check_launch(native_command, native_environment, fixture.path(), arguments, original_path, "native");
            check_powershell_launches(windows_powershell, name, native_environment,
                fixture.path(), arguments, original_path, "native");
            if (!optional_powershell.isEmpty()) {
                check_powershell_launches(optional_powershell, name, native_environment,
                    fixture.path(), arguments, original_path, "native");
            }

            // A prompt can approach the native command-line limit without an encoded-command size penalty.
            const QStringList long_arguments{"--prompt", QString(16000, QLatin1Char('p'))};
            QStringList long_command = QStringList{fixture.filePath(name + ".exe")} + long_arguments;
            QProcessEnvironment long_environment = environment;
            long_environment.insert("PATH", original_path);
            require(commands.prepare(long_command, long_environment, error), error);
            check_launch(long_command, long_environment,
                fixture.path(), long_arguments, original_path, "native");
            const QStringList long_shell{
                windows_powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                powershell_invocation(name, long_arguments)};
            check_launch(long_shell, long_environment,
                fixture.path(), long_arguments, original_path, "native");
            if (!optional_powershell.isEmpty()) {
                QStringList optional_long_shell = long_shell;
                optional_long_shell.front() = optional_powershell;
                check_launch(optional_long_shell, long_environment,
                    fixture.path(), long_arguments, original_path, "native");
            }

            const QString native_cmd_tail = cmd_tail +
                " unquoted^&value unquoted^|value unquoted^^value ^(unquoted-parentheses^)";
            const QStringList cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + native_cmd_tail};
            const QStringList native_cmd_arguments = original_cmd_arguments(
                cmd, native_environment, other_directory.path(), original_path);
            check_launch(cmd,
                native_environment, other_directory.path(), native_cmd_arguments, original_path, "native");

            // cmd searches cwd before PATH and uses the caller's PATHEXT order for PATH matches.
            QProcessEnvironment cmd_environment = native_environment;
            cmd_environment.insert("PATHEXT", ".CMD;.EXE;.BAT;.COM");
            const QStringList wrapper_cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + cmd_tail};
            const QStringList cmd_arguments = original_cmd_arguments(
                wrapper_cmd, cmd_environment, other_directory.path(), original_path);
            check_launch(wrapper_cmd,
                cmd_environment, other_directory.path(), cmd_arguments, original_path, "cmd");
            require(QFile::copy(executable, other_directory.filePath(name + ".exe")), "copy cwd native fixture");
            const QStringList cwd_cmd{
                parent.value("COMSPEC"), "/d", "/v:off", "/s", "/c",
                "\"\"" + QDir::toNativeSeparators(QDir(private_path).filePath(name + ".exe")) +
                    "\"" + native_cmd_tail + "\""};
            check_launch(cwd_cmd,
                cmd_environment, other_directory.path(), native_cmd_arguments, original_path, "native");
        }
#else
        check_launch(shell + QStringList{"-c", "codex-pet \"$@\"", "fixture"} + arguments,
            pet_environment, fixture.path(), arguments, original_path, expected_wrapper);
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
        const QString executable_name = QFileInfo(application.applicationFilePath()).completeBaseName();
        if (arguments.value(0) == "--fixture" || arguments.value(0) == "--fixture-encoded" ||
            executable_name == "codex" || executable_name == "codex-pet" ||
            executable_name == "verified-codex" || executable_name == "node")
        {
            return run_fixture(arguments, executable_name == "node");
        }
        run_tests();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
