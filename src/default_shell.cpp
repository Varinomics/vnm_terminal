#include "vnm_terminal/default_shell.h"

#include <QString>
#include <QtGlobal>

#if defined(__linux__)
#include <cerrno>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <vector>
#endif

namespace vnm_terminal {

namespace {

QString environment_or_default(const char* name, const QString& fallback)
{
    const QString value = QString::fromLocal8Bit(qgetenv(name));
    return value.trimmed().isEmpty() ? fallback : value;
}

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

QStringList default_shell_argv()
{
#if defined(_WIN32)
    return {environment_or_default("COMSPEC", QStringLiteral("cmd.exe"))};
#elif defined(__linux__)
    const QString environment_shell = environment_or_default("SHELL", {});
    if (!environment_shell.isEmpty()) {
        return {environment_shell};
    }

    const QString account_shell = current_account_login_shell();
    return {account_shell.isEmpty() ? QStringLiteral("/bin/sh") : account_shell};
#elif defined(__APPLE__)
    return {environment_or_default("SHELL", QStringLiteral("/bin/sh"))};
#else
    return {};
#endif
}

} // namespace vnm_terminal
