#include "../../src/app_clipboard_reader.cpp"

#include "helpers/test_check.h"

#include <QEventLoop>

#include <cstdlib>
#include <memory>

namespace chrome = vnm_terminal::terminal_app;

namespace {

using vnm_terminal::test_helpers::check;

const QByteArray k_sample_text = QByteArrayLiteral("alpha \xce\xbb\r\nbeta");

int run_fixture(const QString& mode)
{
    if (!chrome::set_standard_output_binary()) {
        return 3;
    }
    if (mode == QStringLiteral("hang")) {
        return QCoreApplication::exec();
    }

    QByteArray output;
    FILE* destination = stdout;
    if (mode == QStringLiteral("text")) {
        output = k_sample_text;
    }
    else
    if (mode == QStringLiteral("invalid_utf8")) {
        output = QByteArrayLiteral("\xc3");
    }
    else
    if (mode == QStringLiteral("overflow")) {
        output = QByteArray(9, 'a');
    }
    else
    if (mode == QStringLiteral("stderr_overflow")) {
        output = QByteArray(chrome::k_clipboard_broker_maximum_error_bytes + 1, 'e');
        destination = stderr;
    }
    else
    if (mode == QStringLiteral("failure")) {
        output = QByteArrayLiteral("partial output");
    }

    if (!output.isEmpty()) {
        const auto size = (std::size_t)output.size();
        if (std::fwrite(output.constData(), 1, size, destination) != size) {
            return 3;
        }
    }
    if (std::fflush(destination) != 0) {
        return 3;
    }
    return mode == QStringLiteral("failure") ? 7 : 0;
}

bool await_destruction(QObject* object)
{
    QEventLoop loop;
    QTimer watchdog;
    bool destroyed = false;
    watchdog.setSingleShot(true);
    QObject::connect(object, &QObject::destroyed, &loop, [&] {
        destroyed = true;
        loop.quit();
    });
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(15000);
    loop.exec();
    return check(destroyed, "broker request is reclaimed after its process stops");
}

bool test_result(const QString& mode, std::optional<QString> expected, bool missing_program = false)
{
    auto context = std::make_unique<QObject>();
    int completions = 0;
    std::optional<QString> received;
    const QString program = missing_program
        ? QCoreApplication::applicationFilePath() + QStringLiteral(".missing")
        : QCoreApplication::applicationFilePath();
    auto* request = new chrome::Clipboard_broker_request(
        *context, program, {QStringLiteral("--broker-fixture"), mode},
        mode == QStringLiteral("overflow") ? 8 : chrome::k_clipboard_broker_maximum_bytes,
        [&](std::optional<QString> text) {
            ++completions;
            received = std::move(text);
            context.reset();
        });

    bool ok = check(completions == 0, "completion is deferred until after reader returns");
    ok &= await_destruction(request);
    ok &= check(completions == 1, "broker completes exactly once");
    ok &= check(received == expected, "broker returns expected text or explicit failure");
    return ok;
}

bool test_cancellation(bool destroy_context, bool before_start)
{
    auto context = std::make_unique<QObject>();
    int completions = 0;
    QPointer<chrome::Clipboard_broker_request> request = new chrome::Clipboard_broker_request(
        *context, QCoreApplication::applicationFilePath(),
        {QStringLiteral("--broker-fixture"), QStringLiteral("hang")},
        chrome::k_clipboard_broker_maximum_bytes,
        [&](std::optional<QString>) { ++completions; });
    bool cancellation_ran = false;
    const auto cancel = [&] {
        cancellation_ran = true;
        if (destroy_context) {
            context.reset();
        }
        else {
            request->cancel();
            request->cancel();
        }
    };
    if (before_start) {
        cancel();
    }
    else {
        QObject::connect(request->findChild<QProcess*>(), &QProcess::started,
            request.data(), cancel, Qt::QueuedConnection);
    }
    bool ok = await_destruction(request.data());
    ok &= check(cancellation_ran, "cancellation is exercised at the requested process state");
    ok &= check(completions == 0, "cancellation and context destruction suppress completion");
    ok &= check(request.isNull(), "cancelled request retains no running QProcess");
    return ok;
}

bool test_timeout_keeps_event_loop_live()
{
    QObject context;
    bool heartbeat = false;
    int completions = 0;
    bool failed = false;
    bool heartbeat_before_completion = false;
    auto* request = new chrome::Clipboard_broker_request(
        context, QCoreApplication::applicationFilePath(),
        {QStringLiteral("--broker-fixture"), QStringLiteral("hang")},
        chrome::k_clipboard_broker_maximum_bytes,
        [&](std::optional<QString> text) {
            ++completions;
            failed = !text.has_value();
            heartbeat_before_completion = heartbeat;
        });
    QTimer::singleShot(0, &context, [&] { heartbeat = true; });
    bool ok = await_destruction(request);
    ok &= check(heartbeat, "GUI event loop progresses while broker is pending");
    ok &= check(heartbeat_before_completion, "event loop progresses before broker times out");
    ok &= check(completions == 1 && failed, "hung broker produces one failure and is reaped");
    return ok;
}

bool test_public_reader()
{
    QObject context;
#if !defined(Q_OS_WIN)
    QGuiApplication::clipboard()->setText(QString::fromUtf8(k_sample_text));
#endif
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    int completions = 0;
    std::optional<QString> received;
    const auto cancel = chrome::read_clipboard_text_with_broker(&context,
        [&](std::optional<QString> text) {
            ++completions;
            received = std::move(text);
            loop.quit();
        });
    bool ok = check(completions == 0, "public reader never completes inline");
    watchdog.start(15000);
    loop.exec();
    cancel();
    cancel();
    ok &= check(completions == 1, "public reader completes once");
    ok &= check(received == QString::fromUtf8(k_sample_text), "public reader supplies text");

    const auto cancelled = chrome::read_clipboard_text_with_broker(&context,
        [&](std::optional<QString>) { ++completions; });
    cancelled();
    cancelled();
    QCoreApplication::processEvents();
    ok &= check(completions == 1, "public cancellation suppresses queued startup/completion");
    return ok;
}

int run_shutdown_fixture()
{
    QObject context;
    auto* request = new chrome::Clipboard_broker_request(
        context, QCoreApplication::applicationFilePath(),
        {QStringLiteral("--broker-fixture"), QStringLiteral("hang")},
        chrome::k_clipboard_broker_maximum_bytes,
        [](std::optional<QString>) {
            check(false, "shutdown suppresses pending clipboard completion");
            std::exit(9);
        });
    QObject::connect(request->findChild<QProcess*>(), &QProcess::started,
        QCoreApplication::instance(), &QCoreApplication::quit, Qt::QueuedConnection);
    return QCoreApplication::exec();
}

bool test_shutdown()
{
    QObject context;
    int completions = 0;
    bool succeeded = false;
    auto* request = new chrome::Clipboard_broker_request(
        context, QCoreApplication::applicationFilePath(), {QStringLiteral("--shutdown-fixture")},
        chrome::k_clipboard_broker_maximum_bytes,
        [&](std::optional<QString> text) {
            ++completions;
            succeeded = text.has_value();
        });
    bool ok = await_destruction(request);
    ok &= check(completions == 1 && succeeded, "application exits with a pending broker and no callback");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (chrome::clipboard_broker_mode_requested(arguments)) {
        return run_fixture(QStringLiteral("text"));
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--broker-fixture")) {
        return run_fixture(arguments.at(2));
    }
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--shutdown-fixture")) {
        return run_shutdown_fixture();
    }

    bool ok = true;
    ok &= test_result(QStringLiteral("text"), QString::fromUtf8(k_sample_text));
    ok &= test_result(QStringLiteral("empty"), QString{});
    ok &= test_result(QStringLiteral("failure"), std::nullopt);
    ok &= test_result(QStringLiteral("invalid_utf8"), std::nullopt);
    ok &= test_result(QStringLiteral("overflow"), std::nullopt);
    ok &= test_result(QStringLiteral("stderr_overflow"), std::nullopt);
    ok &= test_result(QStringLiteral("text"), std::nullopt, true);
    ok &= test_cancellation(false, true);
    ok &= test_cancellation(false, false);
    ok &= test_cancellation(true, true);
    ok &= test_cancellation(true, false);
    ok &= test_timeout_keeps_event_loop_live();
    ok &= test_public_reader();
    ok &= test_shutdown();
    return ok ? 0 : 1;
}
