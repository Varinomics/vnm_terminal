#include "app_clipboard_reader.h"

#include "vnm_terminal/app_support/diagnostic_sink.h"

#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringDecoder>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>
#include <mutex>
#include <optional>
#include <utility>

namespace vnm_terminal::terminal_app {

namespace {

constexpr int k_clipboard_broker_timeout_ms = 2000;
constexpr qsizetype k_clipboard_broker_maximum_bytes = 8 * 1024 * 1024;
constexpr qsizetype k_clipboard_broker_maximum_error_bytes = 64 * 1024;

std::mutex& diagnostic_sink_mutex()
{
    static std::mutex mutex;
    return mutex;
}

Diagnostic_sink& configured_diagnostic_sink()
{
    static Diagnostic_sink sink;
    return sink;
}

const char* diagnostic_level_name(Diagnostic_level level)
{
    switch (level) {
        case Diagnostic_level::WARNING: return "warning";
        case Diagnostic_level::ERROR:   return "error";
    }
    return "error";
}

} // namespace

void set_diagnostic_sink(Diagnostic_sink sink)
{
    const std::lock_guard<std::mutex> lock(diagnostic_sink_mutex());
    configured_diagnostic_sink() = std::move(sink);
}

void clear_diagnostic_sink()
{
    set_diagnostic_sink({});
}

void write_diagnostic(Diagnostic_level level, QStringView message)
{
    Diagnostic_sink sink;
    {
        const std::lock_guard<std::mutex> lock(diagnostic_sink_mutex());
        sink = configured_diagnostic_sink();
    }

    if (sink) {
        try {
            sink(level, message);
            return;
        }
        catch (...) {
        }
    }

    const QByteArray utf8 = message.toString().toUtf8();
    std::fprintf(
        stderr,
        "[vnm_terminal][%s] %s\n",
        diagnostic_level_name(level),
        utf8.constData());
    std::fflush(stderr);
}

namespace {

QString internal_clipboard_read_argument()
{
    return QStringLiteral("--vnm-terminal-internal-read-clipboard-text");
}

class Clipboard_broker_request final : public QObject
{
public:
    Clipboard_broker_request(
        QObject& context,
        QString program,
        QStringList arguments,
        qsizetype maximum_bytes,
        Clipboard_completion completion)
    :
        m_context(&context),
        m_process(this),
        m_deadline(this),
        m_maximum_bytes(maximum_bytes),
        m_completion(std::move(completion))
    {
        Q_ASSERT(context.thread() == QThread::currentThread());
        Q_ASSERT(maximum_bytes > 0);
        Q_ASSERT(m_completion);

        m_process.setProgram(program);
        m_process.setArguments(arguments);
        m_process.setProcessChannelMode(QProcess::SeparateChannels);
        m_process.setStandardInputFile(QProcess::nullDevice());
        m_deadline.setSingleShot(true);
        m_deadline.setTimerType(Qt::PreciseTimer);

        connect(&context, &QObject::destroyed, this, &Clipboard_broker_request::cancel);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &Clipboard_broker_request::cancel);
        connect(&m_process, &QProcess::started, this, [this] {
            if (m_settled) {
                m_process.kill();
            }
        });
        connect(&m_process, &QProcess::readyReadStandardOutput,
            this, &Clipboard_broker_request::read_output);
        connect(&m_process, &QProcess::readyReadStandardError,
            this, &Clipboard_broker_request::read_error_output);
        connect(&m_process, &QProcess::finished,
            this, &Clipboard_broker_request::process_finished);
        connect(&m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
                finish(std::nullopt,
                    QStringLiteral("Clipboard broker failed: %1").arg(m_process.errorString()));
            });
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            finish(std::nullopt, QStringLiteral("Clipboard broker timed out."));
        });

        // Queue startup so even FailedToStart cannot complete before the caller
        // has received and stored its cancellation handle.
        QTimer::singleShot(0, this, [this] {
            if (m_settled) {
                return;
            }
            m_deadline.start(k_clipboard_broker_timeout_ms);
            m_process.start(QIODevice::ReadOnly);
        });
    }

    void cancel()
    {
        Q_ASSERT(thread() == QThread::currentThread());
        m_settled = true;
        m_completion = {};
        m_deadline.stop();
        retire_process();
    }

private:
    ~Clipboard_broker_request() override
    {
        Q_ASSERT(m_process.state() == QProcess::NotRunning);
    }

    void retire_process()
    {
        if (m_process.state() == QProcess::NotRunning) {
            deleteLater();
            return;
        }

        // QProcess destruction waits for termination. Keep this parentless
        // request alive until finished, even if the surface has gone away.
        // At application exit kill still runs, but the OS may reclaim the
        // request because the event loop no longer delivers finished.
        m_process.closeReadChannel(QProcess::StandardOutput);
        m_process.closeReadChannel(QProcess::StandardError);
        m_process.kill();
    }

    void finish(std::optional<QString> text, QString error)
    {
        if (m_settled) {
            retire_process();
            return;
        }

        m_settled = true;
        m_deadline.stop();
        Clipboard_completion completion = std::move(m_completion);
        retire_process();
        if (m_context) {
            completion(std::move(text));
        }
        if (!error.isEmpty()) {
            write_diagnostic(Diagnostic_level::WARNING, error);
        }
    }

    bool read_channel(QProcess::ProcessChannel channel, QByteArray& output, qsizetype limit)
    {
        if (m_settled) {
            return false;
        }

        m_process.setReadChannel(channel);
        const qint64 available = m_process.bytesAvailable();
        if (available > limit - output.size()) {
            finish(std::nullopt, QStringLiteral("Clipboard broker output exceeds the size limit."));
            return false;
        }
        if (available > 0) {
            output += m_process.read(available);
        }
        return true;
    }

    void read_output()
    {
        (void)read_channel(QProcess::StandardOutput, m_output, m_maximum_bytes);
    }

    void read_error_output()
    {
        (void)read_channel(QProcess::StandardError, m_error_output, k_clipboard_broker_maximum_error_bytes);
    }

    void process_finished(int exit_code, QProcess::ExitStatus exit_status)
    {
        if (m_settled) {
            retire_process();
            return;
        }

        if (!read_channel(QProcess::StandardOutput, m_output, m_maximum_bytes)) {
            return;
        }
        if (!read_channel(QProcess::StandardError, m_error_output, k_clipboard_broker_maximum_error_bytes)) {
            return;
        }

        if (exit_status != QProcess::NormalExit || exit_code != 0) {
            finish(std::nullopt,
                QStringLiteral("Clipboard broker exited with status %1, code %2: %3")
                    .arg((int)exit_status)
                    .arg(exit_code)
                    .arg(QString::fromLocal8Bit(m_error_output)));
            return;
        }

        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        QString text = decoder.decode(m_output);
        if (decoder.hasError()) {
            finish(std::nullopt, QStringLiteral("Clipboard broker returned invalid UTF-8."));
            return;
        }
        finish(std::move(text), {});
    }

    QPointer<QObject>     m_context;
    QProcess             m_process;
    QTimer               m_deadline;
    qsizetype            m_maximum_bytes;
    Clipboard_completion m_completion;
    QByteArray           m_output;
    QByteArray           m_error_output;
    bool                 m_settled = false;
};

std::optional<QString> read_clipboard_text_directly()
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        write_diagnostic(
            Diagnostic_level::WARNING,
            QStringLiteral("vnm_terminal: no application clipboard is available"));
        return std::nullopt;
    }

    return clipboard->text(QClipboard::Clipboard);
}

bool set_standard_output_binary()
{
#if defined(Q_OS_WIN)
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        write_diagnostic(
            Diagnostic_level::WARNING,
            QStringLiteral("vnm_terminal: failed to switch clipboard broker stdout to binary mode"));
        return false;
    }
#endif
    return true;
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

    if (text->size() > k_clipboard_broker_maximum_bytes) {
        write_diagnostic(Diagnostic_level::WARNING,
            QStringLiteral("Clipboard text exceeds the broker size limit."));
        return 4;
    }
    const QByteArray bytes = text->toUtf8();
    if (bytes.size() > k_clipboard_broker_maximum_bytes) {
        write_diagnostic(Diagnostic_level::WARNING,
            QStringLiteral("Clipboard text exceeds the broker size limit."));
        return 4;
    }
    if (!bytes.isEmpty()) {
        const std::size_t byte_count = static_cast<std::size_t>(bytes.size());
        const std::size_t written =
            std::fwrite(bytes.constData(), 1U, byte_count, stdout);
        if (written != byte_count) {
            write_diagnostic(
                Diagnostic_level::WARNING,
                QStringLiteral("vnm_terminal: failed to write clipboard broker output"));
            return 3;
        }
    }
    if (std::fflush(stdout) != 0) {
        write_diagnostic(
            Diagnostic_level::WARNING,
            QStringLiteral("vnm_terminal: failed to flush clipboard broker output"));
        return 3;
    }

    return 0;
}

Clipboard_cancel read_clipboard_text_with_broker(QObject* context, Clipboard_completion completion)
{
    Q_ASSERT(context != nullptr);
    Q_ASSERT(context->thread() == QThread::currentThread());
    Q_ASSERT(completion);
#if defined(Q_OS_WIN)
    const QPointer<Clipboard_broker_request> request = new Clipboard_broker_request(
        *context,
        QCoreApplication::applicationFilePath(),
        {internal_clipboard_read_argument()},
        k_clipboard_broker_maximum_bytes,
        std::move(completion));
    return [request] {
        if (request) {
            request->cancel();
        }
    };
#else
    const QPointer<QObject> request = new QObject(context);
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
        request.data(), [request] { delete request.data(); });
    QTimer::singleShot(0, request.data(), [request, completion = std::move(completion)]() mutable {
        std::optional<QString> text = read_clipboard_text_directly();
        if (!request) {
            return;
        }
        Clipboard_completion deliver = std::move(completion);
        delete request.data();
        deliver(std::move(text));
    });
    return [request] { delete request.data(); };
#endif
}

} // namespace vnm_terminal::terminal_app
