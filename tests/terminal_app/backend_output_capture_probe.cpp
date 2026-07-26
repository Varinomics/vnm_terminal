#include "app_options.h"

#include "vnm_terminal/backend_output_capture.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QStringList>

#include <iostream>

namespace chrome = vnm_terminal::terminal_app;

namespace {

bool clear_capture(const QString& base_path)
{
    const QFileInfo base_info(base_path);
    if (base_info.exists()) {
        if (!base_info.isFile() || !QFile::remove(base_info.absoluteFilePath())) {
            std::cerr
                << "failed to remove capture base path "
                << base_info.absoluteFilePath().toStdString()
                << '\n';
            return false;
        }
    }

    const QDir parent = base_info.absoluteDir();
    const QFileInfoList entries = parent.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        const vnm_terminal::Backend_output_capture_artifact_inspection inspection =
            vnm_terminal::inspect_backend_output_capture_artifact(
                base_info.absoluteFilePath(),
                entry.absoluteFilePath());
        if (inspection.recognized() && !QFile::remove(entry.absoluteFilePath())) {
            std::cerr
                << "failed to remove capture artifact "
                << entry.absoluteFilePath().toStdString()
                << '\n';
            return false;
        }
    }

    return true;
}

bool check_capture(
    const QString& base_path,
    const QString& expected_text)
{
    const vnm_terminal::Backend_output_capture_config config{
        base_path,
        chrome::k_backend_output_capture_max_bytes,
    };
    const vnm_terminal::Backend_output_capture_recovery recovery =
        vnm_terminal::recover_backend_output_capture(config);
    if (!recovery.valid()) {
        std::cerr
            << "capture recovery failed: "
            << recovery.error.toStdString()
            << '\n';
        return false;
    }
    if (recovery.status != vnm_terminal::Backend_output_capture_status::FINALIZED) {
        std::cerr << "capture did not finalize cleanly\n";
        return false;
    }
    if (recovery.retained_bytes > config.max_bytes) {
        std::cerr << "capture exceeds its configured byte bound\n";
        return false;
    }

    QByteArray captured_bytes;
    for (const vnm_terminal::Backend_output_capture_segment& segment : recovery.segments) {
        QFile file(segment.path);
        if (!file.open(QIODevice::ReadOnly)) {
            std::cerr
                << "failed to read capture segment "
                << segment.path.toStdString()
                << ": "
                << file.errorString().toStdString()
                << '\n';
            return false;
        }
        captured_bytes += file.readAll();
    }

    if (!captured_bytes.contains(expected_text.toUtf8())) {
        std::cerr
            << "capture does not contain expected text: "
            << expected_text.toStdString()
            << '\n';
        return false;
    }

    return true;
}

void print_usage()
{
    std::cerr
        << "usage: vnm_terminal_backend_output_capture_probe "
           "clear <base-path>\n"
        << "       vnm_terminal_backend_output_capture_probe "
           "check <base-path> <expected-text>\n";
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 3 && arguments[1] == QStringLiteral("clear")) {
        return clear_capture(arguments[2]) ? 0 : 1;
    }
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("check")) {
        return check_capture(arguments[2], arguments[3]) ? 0 : 1;
    }

    print_usage();
    return 2;
}
