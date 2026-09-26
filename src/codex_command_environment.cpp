#include "codex_command_environment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <winver.h>
#endif

#include <string>
#include <vector>

namespace vnm_terminal::terminal_app {
namespace {

bool names_codex(const QString& program)
{
    const QString name = QFileInfo(program).fileName();
#if defined(Q_OS_WIN)
    return
        name.compare(QStringLiteral("codex"),     Qt::CaseInsensitive) == 0 ||
        name.compare(QStringLiteral("codex.exe"), Qt::CaseInsensitive) == 0 ||
        name.compare(QStringLiteral("codex.cmd"), Qt::CaseInsensitive) == 0 ||
        name.compare(QStringLiteral("codex.ps1"), Qt::CaseInsensitive) == 0;
#else
    return name == QStringLiteral("codex");
#endif
}

#if defined(Q_OS_WIN)
bool supports_native_arguments(const QString& executable)
{
    const std::wstring path = QDir::toNativeSeparators(executable).toStdWString();
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0) {
        return false;
    }
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return false;
    }
    VS_FIXEDFILEINFO* version = nullptr;
    UINT version_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &version_size) ||
        version_size < sizeof(VS_FIXEDFILEINFO))
    {
        return false;
    }
    const unsigned major = HIWORD(version->dwFileVersionMS);
    const unsigned minor = LOWORD(version->dwFileVersionMS);
    return major > 7 || (major == 7 && minor >= 3);
}
#endif

} // namespace

bool Codex_command_environment::prepare(
    QStringList& command, QProcessEnvironment& environment, QString& error)
{
    const bool direct_codex = !command.isEmpty() && names_codex(command.front());
    const auto fail_setup = [&](const QString& detail) {
        error = direct_codex ? detail : QString();
        return !direct_codex;
    };
    const QString path = environment.value(QStringLiteral("PATH"));
#if defined(Q_OS_WIN)
    const QString powershell = QStandardPaths::findExecutable(
        QStringLiteral("pwsh.exe"), path.split(QDir::listSeparator(), Qt::KeepEmptyParts));
    if (powershell.isEmpty() || !supports_native_arguments(powershell)) {
        return fail_setup(QStringLiteral(
            "Automatic Codex SIXEL support requires PowerShell 7.3 or newer on PATH."));
    }
    const QStringList files = {
        QStringLiteral("codex.ps1"),
        QStringLiteral("codex.cmd"),
        QStringLiteral("launch_codex.ps1"),
        QStringLiteral("start_codex.ps1"),
    };
#else
    const QStringList files = {QStringLiteral("codex"), QStringLiteral("launch_codex.sh")};
#endif
    if (!m_directory.isValid()) {
        return fail_setup(QStringLiteral("Could not prepare terminal-local Codex commands: %1")
            .arg(m_directory.errorString()));
    }
    for (const QString& name : files) {
        QFile resource(QStringLiteral(":/vnm_terminal/codex/") + name);
        if (!resource.open(QIODevice::ReadOnly)) {
            return fail_setup(QStringLiteral("Could not read the bundled Codex command: %1").arg(name));
        }
        QByteArray content = resource.readAll();
#if defined(Q_OS_WIN)
        if (name == QStringLiteral("codex.cmd")) {
            QString escaped_path = QDir::toNativeSeparators(powershell);
            escaped_path.replace(QLatin1Char('%'), QStringLiteral("%%"));
            content.replace("@POWERSHELL@", escaped_path.toUtf8());
        }
#endif
        QFile output(m_directory.filePath(name));
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size()) {
            return fail_setup(QStringLiteral("Could not write the terminal-local Codex command: %1")
                .arg(output.errorString()));
        }
        output.close();
#if !defined(Q_OS_WIN)
        if (!output.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            return fail_setup(QStringLiteral("Could not make the terminal-local Codex command executable."));
        }
#endif
    }
    environment.insert(QStringLiteral("PATH"), m_directory.path() + QDir::listSeparator() + path);
    if (direct_codex) {
#if defined(Q_OS_WIN)
        command = QStringList{
            powershell,
            QStringLiteral("-NoLogo"),
            QStringLiteral("-NoProfile"),
            QStringLiteral("-File"),
            m_directory.filePath(QStringLiteral("start_codex.ps1")),
        } + command;
#else
        command = QStringList{
            QStringLiteral("/bin/sh"),
            m_directory.filePath(QStringLiteral("launch_codex.sh")),
        } + command;
#endif
    }
    return true;
}

} // namespace vnm_terminal::terminal_app
