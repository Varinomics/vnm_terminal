#include "vnm_terminal/app_support/clipboard_reader.h"
#include "vnm_terminal/internal/backend_contract.h"
#include "vnm_terminal/internal/terminal_input_encoder.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QProcess>
#include <QQuickWindow>
#include <QTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chrome = vnm_terminal::terminal_app;
namespace term = vnm_terminal::internal;

namespace {

constexpr char k_explicit_broker_text[] = "explicit broker fixture";

QString broker_program()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("vnm_terminal_clipboard_broker.exe"));
}

bool put_clipboard_format(UINT format, const QByteArray& bytes)
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
    if (!memory) {
        return false;
    }
    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(data, bytes.data(), static_cast<std::size_t>(bytes.size()));
    GlobalUnlock(memory);
    if (!SetClipboardData(format, memory)) {
        GlobalFree(memory);
        return false;
    }
    return true; // The clipboard now owns the allocation.
}

bool set_native_clipboard(HWND owner, UINT image_format, bool file_reference, bool real_text)
{
    if (!QTest::qWaitFor([owner] { return OpenClipboard(owner) != FALSE; }, 1000)) {
        qWarning("OpenClipboard failed with error %lu", GetLastError());
        return false;
    }
    bool ok = EmptyClipboard();
    if (ok && image_format == CF_DIB) {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = 1;
        header.biHeight = 1;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biSizeImage = 4;
        QByteArray dib(reinterpret_cast<const char*>(&header), sizeof(header));
        dib.append(QByteArray::fromHex("0000ff00"));
        ok = put_clipboard_format(CF_DIB, dib);
    }
    else
    if (ok && image_format != 0U) {
        QImage image(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        ok = buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") &&
            put_clipboard_format(image_format, png);
    }
    if (ok && file_reference) {
        DROPFILES header{};
        header.pFiles = sizeof(header);
        header.fWide = TRUE;
        const wchar_t path[] = L"C:\\clipboard-fixture.png\0";
        QByteArray drop(reinterpret_cast<const char*>(&header), sizeof(header));
        drop.append(reinterpret_cast<const char*>(path), sizeof(path));
        ok = put_clipboard_format(CF_HDROP, drop);
    }
    if (ok && real_text) {
        const QString text = QStringLiteral("real text \u03bb");
        const QByteArray bytes(reinterpret_cast<const char*>(text.utf16()),
            (text.size() + 1) * sizeof(char16_t));
        ok = put_clipboard_format(CF_UNICODETEXT, bytes);
    }
    return CloseClipboard() && ok;
}

// Capture the actual surface/session encoding while the native clipboard and
// separately launched broker remain real. ConPTY transport is tested elsewhere.
class Recording_backend final : public term::Terminal_backend
{
public:
    explicit Recording_backend(std::shared_ptr<std::vector<QByteArray>> writes)
    :
        m_writes(std::move(writes))
    {}

    term::Terminal_backend_result start(const term::Terminal_launch_config&,
        term::Terminal_backend_callbacks callbacks) override
    {
        m_callbacks = std::move(callbacks);
        return term::backend_accept();
    }
    term::Terminal_backend_result write(QByteArray bytes) override
    {
        m_writes->push_back(std::move(bytes));
        return term::backend_accept();
    }
    term::Terminal_backend_result resize(term::Terminal_backend_resize_request) override
    {
        return term::backend_accept();
    }
    term::Terminal_backend_result set_output_paused(bool) override { return term::backend_accept(); }
    term::Terminal_backend_result interrupt() override { return terminate(); }
    term::Terminal_backend_result terminate() override
    {
        if (auto exited = std::exchange(m_callbacks.process_exited, {})) {
            exited({term::Terminal_exit_reason::TERMINATED, 0});
        }
        return term::backend_accept();
    }

private:
    std::shared_ptr<std::vector<QByteArray>> m_writes;
    term::Terminal_backend_callbacks m_callbacks;
};

} // namespace

class Hosted_clipboard_tests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(QFileInfo::exists(broker_program()));
        m_clipboard_owner.create();
    }

    void cleanup()
    {
        if (std::exchange(m_clipboard_used, false)) {
            QVERIFY(set_native_clipboard(reinterpret_cast<HWND>(m_clipboard_owner.winId()), 0U, false, false));
        }
    }

    void native_formats_data()
    {
        QTest::addColumn<uint>("image_format");
        QTest::addColumn<bool>("file_reference");
        QTest::addColumn<bool>("real_text");
        QTest::addColumn<bool>("image_only");
        const uint png = RegisterClipboardFormatW(L"PNG");
        QVERIFY(png != 0U);
        QTest::newRow("empty") << 0U << false << false << false;
        QTest::newRow("dib") << uint(CF_DIB) << false << false << true;
        QTest::newRow("png") << png << false << false << true;
        QTest::newRow("screenshot-file-reference") << uint(CF_DIB) << true << false << true;
        QTest::newRow("real-text-with-image") << uint(CF_DIB) << true << true << false;
        QTest::newRow("file-reference-only") << 0U << true << false << false;
    }

    void native_formats()
    {
        QFETCH(uint, image_format);
        QFETCH(bool, file_reference);
        QFETCH(bool, real_text);
        QFETCH(bool, image_only);
        m_clipboard_used = true;
        QVERIFY(set_native_clipboard(reinterpret_cast<HWND>(m_clipboard_owner.winId()),
            image_format, file_reference, real_text));
        QObject context;
        int completions = 0;
        int images = 0;
        std::optional<QString> received;
        const auto cancel = chrome::read_clipboard_text_with_broker(&context,
            [&](std::optional<QString> text) { ++completions; received = std::move(text); },
            [&] { ++images; }, broker_program());
        QCOMPARE(completions, 0);
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
        cancel();
        QVERIFY(received.has_value());
        QCOMPARE(images, image_only ? 1 : 0);
        if (real_text) {
            QCOMPARE(*received, QStringLiteral("real text \u03bb"));
        }
        else
        if (file_reference && !image_only) {
            QVERIFY(received->contains(QStringLiteral("clipboard-fixture.png")));
        }
        else {
            QVERIFY(received->isEmpty());
        }
    }

    void installed_surface_paste_data()
    {
        QTest::addColumn<bool>("real_text");
        QTest::newRow("image") << false;
        QTest::newRow("text-precedence") << true;
    }

    void installed_surface_paste()
    {
        QFETCH(bool, real_text);
        m_clipboard_used = true;
        QVERIFY(set_native_clipboard(reinterpret_cast<HWND>(m_clipboard_owner.winId()),
            CF_DIB, true, real_text));
        QQuickWindow window;
        VNM_TerminalSurface surface(window.contentItem());
        surface.setSize(QSizeF(640, 320));
        surface.set_bracketed_paste_policy(VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const auto writes = std::make_shared<std::vector<QByteArray>>();
        QVERIFY(term::VNM_TerminalSurface_render_bridge::start_backend_terminal(surface,
            std::make_unique<Recording_backend>(writes), {QStringLiteral("clipboard-recorder")}).accepted);
        chrome::install_hosted_clipboard_reader(surface);
        QVERIFY(surface.paste_clipboard_text());
        QTRY_COMPARE_WITH_TIMEOUT(writes->size(), std::size_t(1), 10000);
        const QKeyEvent image_shortcut(
            QEvent::KeyPress, Qt::Key_V, Qt::AltModifier, QStringLiteral("v"));
        const QByteArray expected = real_text
            ? QStringLiteral("real text \u03bb").toUtf8()
            : term::encode_terminal_key_event(image_shortcut, {});
        QCOMPARE(writes->front(), expected);
        QCoreApplication::processEvents();
        QCOMPARE(writes->size(), std::size_t(1));

        writes->clear();
        QVERIFY(surface.paste_clipboard_text());
        surface.set_clipboard_text_reader({});
        QCoreApplication::processEvents();
        QVERIFY(writes->empty());
    }

    void explicit_broker_program()
    {
        QQuickWindow window;
        VNM_TerminalSurface surface(window.contentItem());
        surface.setSize(QSizeF(640, 320));
        surface.set_bracketed_paste_policy(VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        const auto writes = std::make_shared<std::vector<QByteArray>>();
        QVERIFY(term::VNM_TerminalSurface_render_bridge::start_backend_terminal(surface,
            std::make_unique<Recording_backend>(writes), {QStringLiteral("clipboard-recorder")}).accepted);
        chrome::install_hosted_clipboard_reader(surface, QCoreApplication::applicationFilePath());
        QVERIFY(surface.paste_clipboard_text());
        QTRY_COMPARE_WITH_TIMEOUT(writes->size(), std::size_t(1), 10000);
        QCOMPARE(writes->front(), QByteArray(k_explicit_broker_text));
    }

    void helper_rejects_ordinary_startup()
    {
        QProcess helper;
        helper.start(broker_program(), {QStringLiteral("--ordinary-host-startup")});
        QVERIFY(helper.waitForStarted());
        QVERIFY(helper.waitForFinished(10000));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), EXIT_FAILURE);
        QVERIFY(helper.readAllStandardOutput().isEmpty());
    }

private:
    QWindow m_clipboard_owner;
    bool m_clipboard_used = false;
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (chrome::clipboard_broker_mode_requested(app.arguments())) {
        // The explicit-program case returns controlled bytes without consulting
        // the native clipboard. Default-program cases use the real helper.
        const std::size_t size = sizeof(k_explicit_broker_text) - 1;
        return std::fwrite(k_explicit_broker_text, 1, size, stdout) == size && std::fflush(stdout) == 0
            ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    Hosted_clipboard_tests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "hosted_clipboard_tests.moc"
