#include "vnm_terminal/default_shell.h"

#include <QByteArray>
#include <QTest>
#include <QtGlobal>

#if defined(__linux__)
#include <cerrno>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <vector>
#endif

namespace {

#if defined(__linux__)
QString current_account_login_shell()
{
    constexpr std::size_t k_default_buffer_size = 16 * 1024;
    const long suggested_buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
    const std::size_t initial_buffer_size = suggested_buffer_size > 0
        ? static_cast<std::size_t>(suggested_buffer_size)
        : k_default_buffer_size;
    std::vector<char> buffer(initial_buffer_size);

    const uid_t user_id = getuid();
    struct passwd account {};
    struct passwd* account_result = nullptr;
    while (true) {
        const int result = getpwuid_r(
            user_id, &account, buffer.data(), buffer.size(), &account_result);
        if (result == ERANGE) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (result != 0 || account_result == nullptr || account.pw_shell == nullptr) {
            return {};
        }

        const QString shell = QString::fromLocal8Bit(account.pw_shell);
        return shell.trimmed().isEmpty() ? QString() : shell;
    }
}
#endif

} // namespace

class Default_shell_tests final : public QObject
{
    Q_OBJECT

private slots:
    void empty_or_absent_environment_uses_platform_default_shell()
    {
#if defined(_WIN32)
        constexpr char variable[] = "COMSPEC";
        const QString fallback = QStringLiteral("cmd.exe");
#elif defined(__linux__)
        constexpr char variable[] = "SHELL";
        const QString account_shell = current_account_login_shell();
        const QString fallback = account_shell.isEmpty()
            ? QStringLiteral("/bin/sh")
            : account_shell;
#elif defined(__APPLE__)
        constexpr char variable[] = "SHELL";
        const QString fallback = QStringLiteral("/bin/sh");
#else
        QCOMPARE(vnm_terminal::default_shell_argv(), QStringList());
        return;
#endif
        const QByteArray original = qgetenv(variable);
        const bool was_set = qEnvironmentVariableIsSet(variable);

        qunsetenv(variable);
        const QStringList absent_environment = vnm_terminal::default_shell_argv();
        qputenv(variable, QByteArray());
        const QStringList empty_environment = vnm_terminal::default_shell_argv();
        qputenv(variable, QByteArray(" \t"));
        const QStringList blank_environment = vnm_terminal::default_shell_argv();

        if (was_set) {
            qputenv(variable, original);
        } else {
            qunsetenv(variable);
        }

        QCOMPARE(absent_environment, QStringList({fallback}));
        QCOMPARE(empty_environment, QStringList({fallback}));
        QCOMPARE(blank_environment, QStringList({fallback}));
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

        if (was_set) {
            qputenv(variable, original);
        } else {
            qunsetenv(variable);
        }

        QCOMPARE(argv.size(), 1);
        QCOMPARE(argv.front(), QString::fromLocal8Bit(candidate));
    }
};

QTEST_GUILESS_MAIN(Default_shell_tests)

#include "default_shell_tests.moc"
