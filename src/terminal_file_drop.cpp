#include "vnm_terminal/app_support/terminal_file_drop.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QDropEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
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
    LITERAL,
    UNSUPPORTED,
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
        return Shell_syntax::CMD;
    }

    if (executable == QStringLiteral("csh") || executable == QStringLiteral("tcsh")) {
        return Shell_syntax::UNSUPPORTED;
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
        case Shell_syntax::LITERAL: {
            bool needs_quotes = path.contains(QLatin1Char('"'));
            for (const QChar character : path) {
                needs_quotes = needs_quotes || character.isSpace();
            }
            if (!needs_quotes) {
                return path;
            }

            QString escaped_path = path;
            escaped_path.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
            escaped_path.replace(QStringLiteral("\""), QStringLiteral("\\\""));
            return QStringLiteral("\"") + escaped_path + QStringLiteral("\"");
        }
        case Shell_syntax::POSIX: {
            QString escaped_path = path;
            escaped_path.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
            return QStringLiteral("'") + escaped_path + QStringLiteral("'");
        }
        case Shell_syntax::POWERSHELL: {
            QString escaped_path = path;
            for (const QChar quote : QStringLiteral("'\u2018\u2019\u201a\u201b")) {
                escaped_path.replace(QString(1, quote), QString(2, quote));
            }
            return QStringLiteral("'") + escaped_path + QStringLiteral("'");
        }
        case Shell_syntax::CMD:
            // cmd expands %NAME% and, with delayed expansion, !NAME!; quotes end the wrapper.
            if (path.contains(QLatin1Char('%')) ||
                path.contains(QLatin1Char('!')) ||
                path.contains(QLatin1Char('"')))
            {
                return std::nullopt;
            }

            return QStringLiteral("\"") + path + QStringLiteral("\"");
        case Shell_syntax::UNSUPPORTED:
            return std::nullopt;
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

        if (event.type() == QEvent::Drop) {
            if (!m_surface.paste_text(*text)) {
                event.ignore();
                return true;
            }

            m_surface.forceActiveFocus(Qt::OtherFocusReason);
            if (QQuickWindow* window = m_surface.window()) {
                window->requestActivate();
            }
        }

        event.setDropAction(Qt::CopyAction);
        event.accept();
        return true;
    }

    VNM_TerminalSurface& m_surface;
    QStringList          m_command;
};

} // namespace

bool terminal_file_drop_is_shell_command(const QStringList& command)
{
    return shell_syntax_for_command(command).has_value();
}

bool terminal_file_drop_supported(const QStringList& command, Terminal_file_drop_mode mode)
{
    if (mode == Terminal_file_drop_mode::LITERAL_TEXT) {
        return true;
    }
    const auto syntax = shell_syntax_for_command(command);
    return syntax && *syntax != Shell_syntax::UNSUPPORTED;
}

std::optional<QString> quote_terminal_paths(
    const QStringList& paths,
    const QStringList& command,
    Terminal_file_drop_mode mode)
{
    const std::optional<Shell_syntax> syntax = mode == Terminal_file_drop_mode::LITERAL_TEXT
        ? Shell_syntax::LITERAL : shell_syntax_for_command(command);
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
    const QStringList& command,
    Terminal_file_drop_mode mode)
{
    if (urls.isEmpty()) {
        return std::nullopt;
    }

    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl& url : urls) {
        if (!url.isValid() || !url.isLocalFile()) {
            return std::nullopt;
        }

        QUrl file_url = url;
        if (file_url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
            file_url.setHost(QString{});
        }
        const QString local_path = file_url.toLocalFile();
        if (local_path.isEmpty() || !QDir::isAbsolutePath(local_path)) {
            return std::nullopt;
        }

        paths.append(local_path);
    }

    return quote_terminal_paths(paths, command, mode);
}

void install_terminal_file_drop(
    VNM_TerminalSurface& surface,
    const QStringList& command)
{
    if (!terminal_file_drop_supported(command)) {
        return;
    }

    surface.setFlag(QQuickItem::ItemAcceptsDrops, true);
    auto* filter = new Terminal_file_drop_filter(surface, command);
    surface.installEventFilter(filter);
}

} // namespace vnm_terminal::terminal_app
