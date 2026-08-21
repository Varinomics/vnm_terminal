#include "standalone_environment.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 12> k_framework_canary_names{
    "VNM_CONTROL_ENDPOINT",
    "VNM_CONTROL_TOKEN",
    "VNM_OWNER_ENDPOINT",
    "VNM_OWNER_TOKEN",
    "VNM_RELAY_ENDPOINT",
    "VNM_RELAY_TOKEN",
    "VNM_BOOTSTRAP_ENDPOINT",
    "VNM_BOOTSTRAP_TOKEN",
    "VNM_INVITATION_TOKEN",
    "VNM_AUTHORIZATION_TOKEN",
    "VNM_WORKER_CONTROL_ENDPOINT",
    "VNM_WORKER_CONTROL_TOKEN",
};

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool has_name(
    const std::vector<vnm_terminal::Terminal_environment_entry>& entries,
    const QString& expected_name)
{
    return std::any_of(
        entries.begin(),
        entries.end(),
        [&expected_name](const auto& entry) {
            return entry.name == expected_name;
        });
}

int run_child_environment_oracle(const QString& marker_path)
{
    bool ok = true;
    const QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    ok &= check(
        environment.value(QStringLiteral("ORDINARY_NAME")) ==
            QStringLiteral("ordinary-value"),
        "ordinary base entry did not reach the standalone child");
    ok &= check(
        environment.contains(QStringLiteral("EMPTY_VALUE")) &&
            environment.value(QStringLiteral("EMPTY_VALUE")).isEmpty(),
        "present-empty base entry did not reach the standalone child");
    for (const std::string_view name : k_framework_canary_names) {
        ok &= check(
            !environment.contains(QString::fromLatin1(
                name.data(),
                static_cast<qsizetype>(name.size()))),
            "reserved credential canary reached the standalone child");
    }
#if defined(Q_OS_WIN)
    ok &= check(
        environment.value(QStringLiteral("=C:")) ==
            QStringLiteral("C:\\explicit-pseudo-directory"),
        "trusted Windows leading-equals pseudo variable did not round-trip");
#endif
    std::cout << "standalone-child-environment-observed\n";
    if (ok) {
        QFile marker(marker_path);
        ok &= check(
            marker.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                marker.write("standalone-child-environment-observed") > 0,
            "standalone child could not write its observation marker");
    }
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const qsizetype child_mode_index = app.arguments().indexOf(
        QStringLiteral("--child-environment-oracle"));
    if (child_mode_index >= 0) {
        if (child_mode_index + 1 >= app.arguments().size()) {
            return 2;
        }
        return run_child_environment_oracle(
            app.arguments().at(child_mode_index + 1));
    }

    bool ok = true;
    const QProcessEnvironment ambient_before =
        QProcessEnvironment::systemEnvironment();

    std::vector<vnm_terminal::Terminal_environment_entry> captured{
        {QStringLiteral("ORDINARY_NAME"), QStringLiteral("ordinary-value")},
        {QStringLiteral("EMPTY_VALUE"), QString()},
    };
    for (const std::string_view name : k_framework_canary_names) {
        captured.push_back({
            QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())),
            QStringLiteral("credential-canary"),
        });
    }

#if defined(Q_OS_WIN)
    captured.push_back({
        QStringLiteral("=C:"),
        QStringLiteral("C:\\explicit-pseudo-directory"),
    });
#endif

    const auto sanitized =
        vnm_terminal::terminal_app::sanitize_standalone_base_environment(
            captured);
    ok &= check(sanitized.has_value(), "explicit captured environment must sanitize");
    if (sanitized.has_value()) {
        ok &= check(
            has_name(*sanitized, QStringLiteral("ORDINARY_NAME")),
            "ordinary captured entries must survive");
        ok &= check(
            has_name(*sanitized, QStringLiteral("EMPTY_VALUE")),
            "empty values must survive");
        for (const std::string_view name : k_framework_canary_names) {
            ok &= check(
                !has_name(
                    *sanitized,
                    QString::fromLatin1(
                        name.data(),
                        static_cast<qsizetype>(name.size()))),
                "framework class-a and class-b canaries must be stripped");
        }
#if defined(Q_OS_WIN)
        ok &= check(
            has_name(*sanitized, QStringLiteral("=C:")),
            "trusted Windows leading-equals pseudo variables must survive");
#endif
    }

    ok &= check(
        QProcessEnvironment::systemEnvironment() == ambient_before,
        "sanitizing an explicit capture must not mutate the ambient environment");

    const auto ambient_capture =
        vnm_terminal::terminal_app::capture_standalone_ambient_environment();
    ok &= check(
        QProcessEnvironment::systemEnvironment() == ambient_before,
        "capturing the ambient environment must not mutate it");
    ok &= check(
        vnm_terminal::terminal_app::sanitize_standalone_base_environment(
            ambient_capture).has_value(),
        "the real ambient capture must satisfy framework policy");

    QTemporaryDir child_working_directory;
    ok &= check(
        child_working_directory.isValid(),
        "standalone child oracle needs a temporary working directory");
    QProcessEnvironment child_parent_environment =
        QProcessEnvironment::systemEnvironment();
    child_parent_environment.insert(
        QStringLiteral("ORDINARY_NAME"),
        QStringLiteral("ordinary-value"));
    child_parent_environment.insert(QStringLiteral("EMPTY_VALUE"), QString{});
    for (const std::string_view name : k_framework_canary_names) {
        child_parent_environment.insert(
            QString::fromLatin1(
                name.data(),
                static_cast<qsizetype>(name.size())),
            QStringLiteral("credential-canary"));
    }
#if defined(Q_OS_WIN)
    child_parent_environment.insert(
        QStringLiteral("=C:"),
        QStringLiteral("C:\\explicit-pseudo-directory"));
#endif

    QProcess standalone_process;
    standalone_process.setProcessEnvironment(child_parent_environment);
    standalone_process.setWorkingDirectory(child_working_directory.path());
    standalone_process.setProgram(
        QString::fromUtf8(VNM_TERMINAL_STANDALONE_TEST_APP_PATH));
    const QString child_marker_path = child_working_directory.filePath(
        QStringLiteral("standalone-child-environment-observed.txt"));
    standalone_process.setArguments({
        QStringLiteral("--native-titlebar"),
        QStringLiteral("--window-size"),
        QStringLiteral("640x360"),
        QStringLiteral("--cwd"),
        child_working_directory.path(),
        QStringLiteral("--timeout-ms"),
        QStringLiteral("10000"),
        QStringLiteral("--require-output"),
        QStringLiteral("--"),
        QCoreApplication::applicationFilePath(),
        QStringLiteral("--child-environment-oracle"),
        child_marker_path,
    });
    standalone_process.start();
    ok &= check(
        standalone_process.waitForStarted(10'000),
        "standalone environment oracle did not start");
    ok &= check(
        standalone_process.waitForFinished(30'000),
        "standalone environment oracle did not finish");
    ok &= check(
        standalone_process.exitStatus() == QProcess::NormalExit &&
            standalone_process.exitCode() == 0,
        "standalone child rejected the effective environment");
    QFile child_marker(child_marker_path);
    ok &= check(
        child_marker.open(QIODevice::ReadOnly) &&
            child_marker.readAll() ==
                QByteArrayLiteral("standalone-child-environment-observed"),
        "standalone child did not report environment observation");
    if (!ok) {
        std::cerr << standalone_process.readAllStandardError().constData();
    }
    ok &= check(
        QProcessEnvironment::systemEnvironment() == ambient_before,
        "standalone launch must not mutate its parent's ambient environment");

    return ok ? 0 : 1;
}
