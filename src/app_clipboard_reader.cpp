#include "app_clipboard_reader.h"

#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>
#include <memory>
#include <optional>

namespace vnm_terminal::terminal_app {

namespace {

constexpr int k_clipboard_broker_timeout_ms = 750;
constexpr int k_clipboard_broker_kill_grace_ms = 50;

QString internal_clipboard_read_argument()
{
    return QStringLiteral("--vnm-terminal-internal-read-clipboard-text");
}

std::optional<QString> read_clipboard_text_directly()
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        qWarning("vnm_terminal: no application clipboard is available");
        return std::nullopt;
    }

    return clipboard->text(QClipboard::Clipboard);
}

bool set_standard_output_binary()
{
#if defined(Q_OS_WIN)
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        qWarning("vnm_terminal: failed to switch clipboard broker stdout to binary mode");
        return false;
    }
#endif
    return true;
}

bool kill_clipboard_broker(QProcess& process)
{
    if (process.state() == QProcess::NotRunning) {
        return true;
    }

    process.kill();
    if (process.waitForFinished(k_clipboard_broker_kill_grace_ms)) {
        return true;
    }

    qWarning(
        "vnm_terminal: clipboard broker did not exit within %d ms after kill",
        k_clipboard_broker_kill_grace_ms);
    return false;
}

void release_running_clipboard_broker(std::unique_ptr<QProcess>& process)
{
    if (process == nullptr || process->state() == QProcess::NotRunning) {
        return;
    }

    QObject::connect(
        process.get(),
        &QProcess::finished,
        process.get(),
        &QObject::deleteLater);
    (void)process.release();
}

} // namespace

bool clipboard_broker_mode_requested(const QStringList& arguments)
{
    return arguments.size() == 2 && arguments.at(1) == internal_clipboard_read_argument();
}

int run_clipboard_text_broker(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (!set_standard_output_binary()) {
        return 3;
    }

    const std::optional<QString> text = read_clipboard_text_directly();
    if (!text.has_value()) {
        return 2;
    }

    const QByteArray bytes = text->toUtf8();
    if (!bytes.isEmpty()) {
        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        const std::size_t written =
            std::fwrite(bytes.constData(), 1U, byte_count, stdout);
        if (written != byte_count) {
            qWarning("vnm_terminal: failed to write clipboard broker output");
            return 3;
        }
    }
    if (std::fflush(stdout) != 0) {
        qWarning("vnm_terminal: failed to flush clipboard broker output");
        return 3;
    }

    return 0;
}

std::optional<QString> read_clipboard_text_with_broker()
{
#if defined(Q_OS_WIN)
    const QString program = QCoreApplication::applicationFilePath();
    if (program.isEmpty()) {
        qWarning("vnm_terminal: cannot start clipboard broker without an application path");
        return std::nullopt;
    }

    std::unique_ptr<QProcess> process = std::make_unique<QProcess>();
    process->setProgram(program);
    process->setArguments({internal_clipboard_read_argument()});
    process->setProcessChannelMode(QProcess::SeparateChannels);

    QElapsedTimer elapsed;
    elapsed.start();
    process->start();
    if (!process->waitForStarted(k_clipboard_broker_timeout_ms)) {
        const QString error_string = process->errorString();
        if (!kill_clipboard_broker(*process)) {
            release_running_clipboard_broker(process);
        }
        qWarning(
            "vnm_terminal: clipboard broker failed to start: %s",
            qPrintable(error_string));
        return std::nullopt;
    }

    QByteArray output;
    QByteArray error_output;
    while (process->state() != QProcess::NotRunning) {
        const qint64 remaining = k_clipboard_broker_timeout_ms - elapsed.elapsed();
        if (remaining <= 0) {
            if (!kill_clipboard_broker(*process)) {
                release_running_clipboard_broker(process);
            }
            qWarning("vnm_terminal: clipboard broker timed out");
            return std::nullopt;
        }

        const bool ready = process->waitForReadyRead(static_cast<int>(remaining));
        output += process->readAllStandardOutput();
        error_output += process->readAllStandardError();
        if (!ready && process->state() == QProcess::NotRunning) {
            break;
        }
    }

    output += process->readAllStandardOutput();
    error_output += process->readAllStandardError();

    if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
        qWarning(
            "vnm_terminal: clipboard broker failed with status %d code %d: %s",
            static_cast<int>(process->exitStatus()),
            process->exitCode(),
            error_output.constData());
        return std::nullopt;
    }

    return QString::fromUtf8(output);
#else
    return read_clipboard_text_directly();
#endif
}

} // namespace vnm_terminal::terminal_app
