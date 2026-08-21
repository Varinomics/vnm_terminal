#include "vnm_terminal/default_shell.h"

#include <QByteArray>
#include <QTest>
#include <QtGlobal>

class Default_shell_tests final : public QObject
{
    Q_OBJECT

private slots:
    void empty_or_absent_environment_uses_platform_fallback()
    {
#if defined(_WIN32)
        constexpr char variable[] = "COMSPEC";
        const QString fallback = QStringLiteral("cmd.exe");
#elif defined(__linux__) || defined(__APPLE__)
        constexpr char variable[] = "SHELL";
        const QString fallback = QStringLiteral("/bin/sh");
#else
        QCOMPARE(vnm_terminal::default_shell_argv(), QStringList());
        return;
#endif
        const QByteArray original = qgetenv(variable);
        const bool was_set = qEnvironmentVariableIsSet(variable);

        qunsetenv(variable);
        QCOMPARE(vnm_terminal::default_shell_argv(), QStringList({fallback}));
        qputenv(variable, QByteArray());
        QCOMPARE(vnm_terminal::default_shell_argv(), QStringList({fallback}));

        if (was_set) {
            qputenv(variable, original);
        } else {
            qunsetenv(variable);
        }
    }

    void environment_value_stays_one_separated_argument()
    {
#if defined(_WIN32)
        constexpr char variable[] = "COMSPEC";
        const QByteArray candidate = "C:\\Program Files\\Shell\\shell.exe /not-an-option";
#elif defined(__linux__) || defined(__APPLE__)
        constexpr char variable[] = "SHELL";
        const QByteArray candidate = "/opt/shell with spaces/bin/sh";
#else
        QSKIP("This platform has no default shell contract.");
#endif
        const QByteArray original = qgetenv(variable);
        const bool was_set = qEnvironmentVariableIsSet(variable);
        qputenv(variable, candidate);

        const QStringList argv = vnm_terminal::default_shell_argv();
        QCOMPARE(argv.size(), 1);
        QCOMPARE(argv.front(), QString::fromLocal8Bit(candidate));

        if (was_set) {
            qputenv(variable, original);
        } else {
            qunsetenv(variable);
        }
    }
};

QTEST_GUILESS_MAIN(Default_shell_tests)

#include "default_shell_tests.moc"
