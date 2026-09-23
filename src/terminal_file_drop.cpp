#include "terminal_file_drop.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QDropEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QObject>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>
#include <utility>

namespace vnm_terminal::terminal_app {

namespace {

enum class Shell_syntax
{
    POSIX,
    POWERSHELL,
    CMD,
};

std::optional<Shell_syntax> shell_syntax_for_command(const QStringList& command)
{
    if (command.isEmpty()) {
        return std::nullopt;
    }

    QString executable = QFileInfo(command.constFirst()).fileName().toLower();
    if (executable.endsWith(QStringLiteral(".exe"))) {
        executable.chop(4);
    }

    if (executable == QStringLiteral("sh")    ||
        executable == QStringLiteral("dash")  ||
        executable == QStringLiteral("bash")  ||
        executable == QStringLiteral("zsh")   ||
        executable == QStringLiteral("ksh")   ||
        executable == QStringLiteral("mksh")  ||
        executable == QStringLiteral("ash"))
    {
        return Shell_syntax::POSIX;
    }

    if (executable == QStringLiteral("pwsh") ||
        executable == QStringLiteral("powershell"))
    {
        return Shell_syntax::POWERSHELL;
    }

    if (executable == QStringLiteral("cmd")) {
        for (qsizetype index = 1; index < command.size(); ++index) {
            const QString argument = command.at(index).toLower();
            if (argument == QStringLiteral("/v:on") ||
                argument == QStringLiteral("/v:1"))
            {
                return std::nullopt;
            }
        }

        return Shell_syntax::CMD;
    }

    return std::nullopt;
}

bool has_unsafe_control_character(const QString& path)
{
    for (const QChar character : path) {
        const ushort code = character.unicode();
        if (code < 0x20U ||
            (code >= 0x7fU && code <= 0x9fU) ||
            code == 0x2028U ||
            code == 0x2029U)
        {
            return true;
        }
    }

    return false;
}

std::optional<QString> quote_path(const QString& path, Shell_syntax syntax)
{
    if (path.isEmpty() || has_unsafe_control_character(path)) {
        return std::nullopt;
    }

    switch (syntax) {
        case Shell_syntax::POSIX: {
            QString escaped_path = path;
            escaped_path.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
            return QStringLiteral("'") + escaped_path + QStringLiteral("'");
        }
        case Shell_syntax::POWERSHELL: {
            QString escaped_path = path;
            escaped_path.replace(QStringLiteral("'"), QStringLiteral("''"));
            escaped_path.replace(
                QString(1, QChar(0x2018)),
                QString(2, QChar(0x2018)));
            escaped_path.replace(
                QString(1, QChar(0x2019)),
                QString(2, QChar(0x2019)));
            return QStringLiteral("'") + escaped_path + QStringLiteral("'");
        }
        case Shell_syntax::CMD:
            // cmd expands %NAME% and, when delayed expansion is enabled, !NAME!.
            if (path.contains(QLatin1Char('%')) || path.contains(QLatin1Char('!'))) {
                return std::nullopt;
            }

            return QStringLiteral("\"") + path + QStringLiteral("\"");
    }

    Q_UNREACHABLE();
    return std::nullopt;
}

class Terminal_file_drop_filter final : public QObject
{
public:
    Terminal_file_drop_filter(
        VNM_TerminalSurface& surface,
        QStringList command)
    :
        QObject(&surface),
        m_surface(surface),
        m_command(std::move(command))
    {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != &m_surface) {
            return false;
        }

        switch (event->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove:
            case QEvent::Drop:
                return handle_drop_event(*static_cast<QDropEvent*>(event));
            default:
                return false;
        }
    }

private:
    bool handle_drop_event(QDropEvent& event)
    {
        const std::optional<QString> text = terminal_drop_text_for_local_urls(
            event.mimeData()->urls(),
            m_command);
        if (!text.has_value() || !event.possibleActions().testFlag(Qt::CopyAction)) {
            event.ignore();
            return true;
        }

        if (event.type() == QEvent::Drop && !m_surface.paste_text(*text)) {
            event.ignore();
            return true;
        }

        event.setDropAction(Qt::CopyAction);
        event.accept();
        return true;
    }

    VNM_TerminalSurface& m_surface;
    QStringList          m_command;
};

} // namespace

std::optional<QString> quote_terminal_paths(
    const QStringList& paths,
    const QStringList& command)
{
    const std::optional<Shell_syntax> syntax = shell_syntax_for_command(command);
    if (!syntax.has_value() || paths.isEmpty()) {
        return std::nullopt;
    }

    QStringList quoted_paths;
    quoted_paths.reserve(paths.size());
    for (const QString& path : paths) {
        std::optional<QString> quoted = quote_path(path, *syntax);
        if (!quoted.has_value()) {
            return std::nullopt;
        }

        quoted_paths.append(std::move(*quoted));
    }

    return quoted_paths.join(QLatin1Char(' '));
}

std::optional<QString> terminal_drop_text_for_local_urls(
    const QList<QUrl>& urls,
    const QStringList& command)
{
    if (urls.isEmpty()) {
        return std::nullopt;
    }

    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl& url : urls) {
        const QString host = url.host();
        if (!url.isLocalFile() ||
            (!host.isEmpty() && host.compare(
                QStringLiteral("localhost"), Qt::CaseInsensitive) != 0))
        {
            return std::nullopt;
        }

        const QString local_path = url.toLocalFile();
        if (local_path.isEmpty() || !QDir::isAbsolutePath(local_path)) {
            return std::nullopt;
        }

#ifdef Q_OS_WIN
        if (local_path.startsWith(QStringLiteral("//")) ||
            local_path.startsWith(QStringLiteral("\\\\")))
        {
            return std::nullopt;
        }
#endif

        const QString path = QDir::cleanPath(local_path);
        paths.append(path);
    }

    return quote_terminal_paths(paths, command);
}

void install_terminal_file_drop(
    VNM_TerminalSurface& surface,
    const QStringList& command)
{
    if (!shell_syntax_for_command(command).has_value()) {
        return;
    }

    surface.setFlag(QQuickItem::ItemAcceptsDrops, true);
    auto* filter = new Terminal_file_drop_filter(surface, command);
    surface.installEventFilter(filter);
}

} // namespace vnm_terminal::terminal_app
