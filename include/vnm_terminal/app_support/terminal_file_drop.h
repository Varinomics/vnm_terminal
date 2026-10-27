#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

enum class Terminal_file_drop_mode
{
    SHELL_COMMAND,
    LITERAL_TEXT,
};

// Shell identity remains known when its dialect cannot be quoted.
bool terminal_file_drop_is_shell_command(const QStringList& command);

bool terminal_file_drop_supported(
    const QStringList& command,
    Terminal_file_drop_mode mode = Terminal_file_drop_mode::SHELL_COMMAND);

std::optional<QString> quote_terminal_paths(
    const QStringList& paths,
    const QStringList& command,
    Terminal_file_drop_mode mode = Terminal_file_drop_mode::SHELL_COMMAND);

std::optional<QString> terminal_drop_text_for_local_urls(
    const QList<QUrl>& urls,
    const QStringList& command,
    Terminal_file_drop_mode mode = Terminal_file_drop_mode::SHELL_COMMAND);

void install_terminal_file_drop(
    VNM_TerminalSurface& surface,
    const QStringList& command);

} // namespace vnm_terminal::terminal_app
