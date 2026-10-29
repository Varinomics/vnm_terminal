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
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using vnm_terminal::Terminal_process_start_request;
using vnm_terminal::terminal_app::Codex_invocation;
using vnm_terminal::terminal_app::prepare_codex_command;

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
        QStringList{"TERM", "PATH", "VNM_WRAPPER", "VNM_POWERSHELL_HOST", "CAPABILITY_VALUE"})
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
    const QString& working_directory,
    const std::optional<QString>& native_arguments = std::nullopt)
{
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setWorkingDirectory(working_directory);
#if !defined(Q_OS_WIN)
    Q_UNUSED(native_arguments);
#endif
#if defined(Q_OS_WIN)
    if (native_arguments) {
        child.setNativeArguments(*native_arguments);
        child.start(command.front(), {});
    }
    else
    if (QFileInfo(command.front()).fileName().compare("cmd.exe", Qt::CaseInsensitive) == 0) {
        // cmd parses command text, not the CRT argv quoting used by QProcess.
        child.setNativeArguments(command.mid(1).join(' '));
        child.start(command.front(), {});
    }
    else
#endif
    {
        // QProcess looks up bare POSIX executables in its own PATH, while the
        // terminal backend uses the explicit child environment.
        const QString program = QStandardPaths::findExecutable(
            command.front(), environment.value("PATH").split(QDir::listSeparator()));
        child.start(program.isEmpty() ? command.front() : program, command.mid(1));
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
    const QString& expected_entry_point = QString(),
    const std::optional<QString>& native_arguments = std::nullopt)
{
    const QJsonObject result = launch_fixture(command, environment, working_directory, native_arguments);
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

Terminal_process_start_request make_request(
    const QStringList& command,
    const QProcessEnvironment& environment,
    const QString& working_directory)
{
    Terminal_process_start_request request;
    request.argv              = command;
    request.working_directory = working_directory;
    for (const QString& name : environment.keys()) {
        request.base_environment.push_back({name, environment.value(name)});
    }
    return request;
}

QProcessEnvironment request_environment(const Terminal_process_start_request& request)
{
    QProcessEnvironment environment;
    for (const auto& entry : request.base_environment) {
        environment.insert(entry.name, entry.value);
    }
    if (request.capability_environment) {
        for (const auto& entry : *request.capability_environment) {
            environment.insert(entry.name, entry.value);
        }
    }
    environment.insert("TERM", request.terminal_identity);
    return environment;
}

void check_direct_launch(
    const Terminal_process_start_request& request,
    const QStringList& arguments,
    const QString& original_path,
    const QString& wrapper,
    const QString& entry_point = QString())
{
    require(request.terminal_identity == "vnm-terminal-sixel", "direct launch identity");
    const QProcessEnvironment environment = request_environment(request);
    require(environment.value("PATH") == original_path, "direct launch PATH");
    check_launch(request.argv, environment, request.working_directory, arguments, original_path, wrapper,
        "input from the terminal\n", QString(), entry_point, request.windows_native_arguments);
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
    for (const QStringList& cli_arguments : {
        codex_arguments, QStringList{}, QStringList{"--prompt", QString(16000, QLatin1Char('p'))}})
    {
        const QStringList command = QStringList{node, entry_point} + cli_arguments;
        QProcessEnvironment base_environment = environment;
        base_environment.remove("TERM_PROGRAM");
        auto request = make_request(command, base_environment, fixture_directory);
        request.capability_environment = std::vector<vnm_terminal::Terminal_environment_entry>{
            {"CAPABILITY_VALUE", "preserved"},
            {"TERM_PROGRAM", "inherited-terminal"},
        };
        require(prepare_codex_command(request, error, Codex_invocation{2}), error);
        require(request.argv.mid(0, 2) == command.mid(0, 2), "verified interpreter prefix changed");
        require(request.argv.mid(4) == cli_arguments, "verified interpreter arguments changed");
        require(!request.windows_native_arguments, "verified interpreter lost structured arguments");
        require(request.capability_environment->size() == 1 &&
            request.capability_environment->front().value == "preserved", "capability contribution changed");
        check_direct_launch(request, cli_arguments, original_path, "native", entry_point);
    }
    {
        const QStringList command = QStringList{native_codex} + codex_arguments;
        auto request = make_request(command, environment, fixture_directory);
        require(prepare_codex_command(request, error, Codex_invocation{1}), error);
        require(request.argv.front() == native_codex, "verified native executable changed");
        require(request.argv.mid(3) == codex_arguments, "verified native arguments changed");
        require(!request.windows_native_arguments, "verified executable lost structured arguments");
        check_direct_launch(request, codex_arguments, original_path, "native");
    }
    {
        auto request = make_request(node_command, environment, fixture_directory);
        require(prepare_codex_command(request, error), error);
        require(request.argv == node_command, "ordinary Node command was adapted as Codex");
        require(request_environment(request) == environment, "ordinary Node environment changed");
        const QJsonObject result = launch_fixture(request.argv, request_environment(request), fixture_directory);
        const QJsonObject fields = result.value("environment").toObject();
        require(result.value("arguments").toArray() == QJsonArray::fromStringList(codex_arguments),
            "ordinary Node arguments changed");
        require(result.value("entry_point").toString() == entry_point, "ordinary Node entry point changed");
        require(fields.value("TERM").toString() == "xterm-256color", "ordinary Node TERM changed");
        require(fields.value("TERM_PROGRAM").toString() == "inherited-terminal", "ordinary Node hint changed");
    }
    for (const qsizetype prefix_size : {qsizetype(-1), qsizetype(0), node_command.size() + 1}) {
        auto request = make_request(node_command, environment, fixture_directory);
        require(!prepare_codex_command(request, error, Codex_invocation{prefix_size}) &&
            !error.isEmpty(), "invalid verified prefix was accepted");
        require(request.argv == node_command && request_environment(request) == environment,
            "invalid verified prefix changed the launch");
    }
    {
        auto request = make_request({"node", entry_point}, environment, fixture_directory);
        require(!prepare_codex_command(request, error, Codex_invocation{2}) &&
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

    // Relocate the installed layout beside the real commands, including spaces and Unicode in its path.
    const QDir installed_commands(QStringLiteral(VNM_TERMINAL_CODEX_COMMANDS_DIR));
    for (const QString& name : installed_commands.entryList(QDir::Files)) {
        require(QFile::copy(installed_commands.filePath(name), fixture.filePath(name)),
            "could not copy installed launcher: " + name);
    }

#if defined(Q_OS_WIN)
    environment.insert("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        write_file(fixture.filePath(name), "#!/bin/sh\nexit 93\n");
        write_powershell_fixture(fixture.filePath(name + ".ps1"), executable);
        write_file(fixture.filePath(name + ".cmd"), (
            "@set VNM_WRAPPER=cmd\r\n@\"" + QDir::toNativeSeparators(executable) +
            "\" --fixture %*\r\n@exit /b %errorlevel%\r\n").toUtf8());
        require(QFile::copy(fixture.filePath(name + ".cmd"), fixture.filePath(name + ".bat")),
            "copy batch fixture");
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
    QString error;
    const QStringList existing_files = QDir(fixture.path()).entryList(QDir::Files);
    auto direct = make_request(QStringList{"codex"} + arguments, environment, fixture.path());
    require(prepare_codex_command(direct, error), error);
    require(QDir(fixture.path()).entryList(QDir::Files) == existing_files, "preparation created files");
    check_direct_launch(direct, arguments, original_path, expected_wrapper);
#if !defined(Q_OS_WIN)
    require(direct.argv.front() == "codex", "POSIX executable changed");
    const QString linked_directory = other_directory.filePath("linked-cwd");
    require(QFile::link(fixture.path(), linked_directory), "could not create cwd symlink");
    require(same_existing_directory(linked_directory, fixture.path()), "cwd symlink identity rejected");
    direct.working_directory = linked_directory;
    check_direct_launch(direct, arguments, original_path, expected_wrapper);
#endif

    for (const QString& name : QStringList{"codex", "codex-pet"}) {
#if defined(Q_OS_WIN)
        const QString program = fixture.filePath(name + ".ps1");
#else
        const QString program = fixture.filePath(name);
#endif
        auto request = make_request(QStringList{program} + arguments, environment, fixture.path());
        require(prepare_codex_command(request, error), error);
        check_direct_launch(request, arguments, original_path, expected_wrapper);
    }
    auto pet_request = make_request(QStringList{"codex-pet"} + arguments, environment, fixture.path());
    require(prepare_codex_command(pet_request, error), error);
    check_direct_launch(pet_request, arguments, original_path, expected_wrapper);

    auto ordinary = make_request(shell, environment, fixture.path());
    ordinary.terminal_identity = "vt100";
    require(prepare_codex_command(ordinary, error), error);
    require(ordinary.argv == shell && ordinary.terminal_identity == "vt100", "ordinary shell changed");
    ordinary.terminal_identity = "xterm-256color";
    require(request_environment(ordinary) == environment, "ordinary shell environment changed");
    auto raw_codex = make_request({"codex"}, environment, fixture.path());
    raw_codex.windows_native_arguments = "--prompt review";
    require(!prepare_codex_command(raw_codex, error) && !error.isEmpty(),
        "direct Codex accepted unstructured arguments");
    require(raw_codex.argv == QStringList{"codex"} &&
        raw_codex.windows_native_arguments == "--prompt review" &&
        request_environment(raw_codex) == environment, "failed preparation changed request");

#if defined(Q_OS_WIN)
    QStringList batch_arguments = arguments;
    batch_arguments.removeAll("line\nbreak");
    batch_arguments += QStringList{
        "explain \"A & B\"", "explain \"A | B\"", "explain \"A > B\"",
        "%PATH%", "!PATH!", "pair%%value", "%PATH%PATH%",
    };
    const QString cmd_tail =
        " \"\" \"with spaces\" --flag \"unicode-\u03bb\" \"C:\\working files\\\\\" "
        "\"amp&ersand\" \"pipe|value\" \"caret^value\" \"bang!value\" \"(parentheses)\" "
        "\"percent%value\" \"dollar$value\" percent%value dollar$value unquoted!value";
    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        for (const QString& extension : QStringList{".cmd", ".bat"}) {
            auto request = make_request(
                QStringList{fixture.filePath(name + extension)} + batch_arguments,
                environment, other_directory.path());
            require(prepare_codex_command(request, error), error);
            require(request.argv.size() == 1 && request.windows_native_arguments,
                "batch entry point did not select its native interpreter");
            check_direct_launch(request, batch_arguments, original_path, "cmd");

            for (const QString& line_break : QStringList{"line\nbreak", "line\rbreak"}) {
                const QStringList command{fixture.filePath(name + extension), line_break};
                auto invalid = make_request(command, environment, other_directory.path());
                require(!prepare_codex_command(invalid, error) && !error.isEmpty(),
                    "batch entry point accepted an argument cmd cannot preserve");
                require(invalid.argv == command && !invalid.windows_native_arguments &&
                    request_environment(invalid) == environment, "batch argument rejection changed request");
            }
        }

        const QStringList cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + cmd_tail};
        const QStringList cmd_arguments = original_cmd_arguments(
            cmd, environment, other_directory.path(), original_path);
        QStringList launcher_cmd = cmd;
        launcher_cmd.back().prepend("vnm-");
        check_launch(launcher_cmd,
            environment, other_directory.path(), cmd_arguments, original_path, "cmd");

        check_powershell_launches(windows_powershell, "vnm-" + name, environment,
            fixture.path(), arguments, original_path, expected_wrapper);
        if (!optional_powershell.isEmpty()) {
            check_powershell_launches(optional_powershell, "vnm-" + name, environment,
                fixture.path(), arguments, original_path, expected_wrapper);
        }
    }

    // npm's installed batch entry point still owns its Node selection and script arguments.
    const QString npm_directory = fixture.filePath("npm installation");
    require(QDir().mkpath(npm_directory), "create npm directory");
    const QString npm_node = QDir(npm_directory).filePath("node.exe");
    require(QFile::copy(executable, npm_node), "copy npm interpreter fixture");
    const QString npm_entry = QDir(npm_directory).filePath("node_modules/@openai/codex/bin/codex.js");
    require(QDir().mkpath(QFileInfo(npm_entry).absolutePath()), "create npm package directory");
    write_file(npm_entry, "// The native fixture observes the npm entry point.\n");
    write_file(QDir(npm_directory).filePath("codex.cmd"),
        "@ECHO off\r\n"
        "SETLOCAL\r\n"
        "IF EXIST \"%~dp0node.exe\" (\r\n"
        "  SET \"_prog=%~dp0node.exe\"\r\n"
        ") ELSE (\r\n"
        "  SET \"_prog=node\"\r\n"
        ")\r\n"
        "\"%_prog%\" \"%~dp0node_modules\\@openai\\codex\\bin\\codex.js\" %*\r\n"
        "exit /b %errorlevel%\r\n");
    QProcessEnvironment npm_environment = environment;
    const QString npm_path = npm_directory + ';' + original_path;
    npm_environment.insert("PATH", npm_path);
    auto npm_request = make_request(QStringList{"codex"} + batch_arguments,
        npm_environment, other_directory.path());
    require(prepare_codex_command(npm_request, error), error);
    check_direct_launch(npm_request, batch_arguments, npm_path, "native", QDir::toNativeSeparators(npm_entry));

    auto native_shell = make_request({parent.value("COMSPEC")}, environment, fixture.path());
    native_shell.windows_native_arguments = "/d /c echo unchanged";
    require(prepare_codex_command(native_shell, error), error);
    require(native_shell.windows_native_arguments == "/d /c echo unchanged" &&
        request_environment(native_shell) == environment, "ordinary native shell changed");

    auto missing = make_request({other_directory.filePath("codex.exe")}, environment, fixture.path());
    require(!prepare_codex_command(missing, error) && !error.isEmpty(), "missing executable was accepted");
    require(request_environment(missing) == environment, "missing executable changed environment");
    QProcessEnvironment missing_interpreter = environment;
    missing_interpreter.insert("PATH", fixture.path());
    missing_interpreter.remove("SystemRoot");
    missing_interpreter.remove("WINDIR");
    auto missing_script_host = make_request({fixture.filePath("codex.ps1")}, missing_interpreter, fixture.path());
    require(!prepare_codex_command(missing_script_host, error) && !error.isEmpty(),
        "missing script interpreter was accepted");
    require(request_environment(missing_script_host) == missing_interpreter,
        "missing interpreter changed environment");

    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        require(QFile::remove(fixture.filePath(name + ".ps1")), "remove PowerShell fixture");
        const QString native_program = fixture.filePath(name + ".exe");
        require(QFile::copy(executable, native_program), "copy native fixture");
        for (const QString& program : QStringList{name, native_program}) {
            auto request = make_request(QStringList{program} + arguments, environment, fixture.path());
            require(prepare_codex_command(request, error), error);
            require(request.argv.front().compare(native_program, Qt::CaseInsensitive) == 0 &&
                !request.windows_native_arguments,
                "native Codex executable did not keep structured arguments");
            check_direct_launch(request, arguments, original_path, "native");
        }
        check_powershell_launches(windows_powershell, "vnm-" + name, environment,
            fixture.path(), arguments, original_path, "native");
        if (!optional_powershell.isEmpty()) {
            check_powershell_launches(optional_powershell, "vnm-" + name, environment,
                fixture.path(), arguments, original_path, "native");
        }

        const QStringList long_arguments{"--prompt", QString(16000, QLatin1Char('p'))};
        auto long_request = make_request(QStringList{native_program} + long_arguments, environment, fixture.path());
        require(prepare_codex_command(long_request, error), error);
        check_direct_launch(long_request, long_arguments, original_path, "native");
        const QStringList long_shell{
            windows_powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
            powershell_invocation("vnm-" + name, long_arguments)};
        check_launch(long_shell, environment, fixture.path(), long_arguments, original_path, "native");
        if (!optional_powershell.isEmpty()) {
            QStringList optional_long_shell = long_shell;
            optional_long_shell.front() = optional_powershell;
            check_launch(optional_long_shell, environment, fixture.path(), long_arguments, original_path, "native");
        }

        const QStringList cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + cmd_tail};
        const QStringList native_cmd_arguments = original_cmd_arguments(
            cmd, environment, other_directory.path(), original_path);
        QStringList launcher_cmd = cmd;
        launcher_cmd.back().prepend("vnm-");
        check_launch(launcher_cmd,
            environment, other_directory.path(), native_cmd_arguments, original_path, "native");

        QProcessEnvironment cmd_environment = environment;
        cmd_environment.insert("PATHEXT", ".CMD;.EXE;.BAT;.COM");
        const QStringList wrapper_cmd{parent.value("COMSPEC"), "/d", "/v:off", "/c", name + cmd_tail};
        const QStringList cmd_arguments = original_cmd_arguments(
            wrapper_cmd, cmd_environment, other_directory.path(), original_path);
        QStringList launcher_wrapper_cmd = wrapper_cmd;
        launcher_wrapper_cmd.back().prepend("vnm-");
        check_launch(launcher_wrapper_cmd,
            cmd_environment, other_directory.path(), cmd_arguments, original_path, "cmd");
        require(QFile::copy(executable, other_directory.filePath(name + ".exe")), "copy cwd native fixture");
        const QStringList cwd_cmd{
            parent.value("COMSPEC"), "/d", "/v:off", "/s", "/c",
            "\"\"" + QDir::toNativeSeparators(fixture.filePath("vnm-" + name + ".cmd")) +
                "\"" + cmd_tail + "\""};
        check_launch(cwd_cmd,
            cmd_environment, other_directory.path(), native_cmd_arguments, original_path, "native");
    }
#else
    for (const QString& name : QStringList{"codex", "codex-pet"}) {
        const QStringList shell_command = shell + QStringList{"-c", "vnm-" + name + " \"$@\"", "fixture"} + arguments;
        check_launch(shell_command, environment, fixture.path(), arguments, original_path, expected_wrapper);
        const QString native_program = other_directory.filePath(name);
        require(QFile::copy(executable, native_program), "copy native fixture");
        auto request = make_request(QStringList{native_program} + arguments, environment, fixture.path());
        require(prepare_codex_command(request, error), error);
        require(request.argv.front() == native_program, "native Codex executable changed");
        check_direct_launch(request, arguments, original_path, "native");
    }
#endif
    require(QProcessEnvironment::systemEnvironment() == parent, "parent environment changed");
}

} // namespace

#if defined(Q_OS_WIN)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
#if defined(Q_OS_WIN)
    // Node and Codex consume the modern CRT argv rules. Qt's Windows argument
    // reader uses CommandLineToArgvW, whose consecutive-quote rules differ.
    QStringList arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.append(QString::fromWCharArray(argv[index]));
    }
    QByteArray application_name = QString::fromWCharArray(argv[0]).toUtf8();
    int application_argc = 1;
    char* application_argv[]{application_name.data(), nullptr};
    QCoreApplication application(application_argc, application_argv);
#else
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
#endif
    try {
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
