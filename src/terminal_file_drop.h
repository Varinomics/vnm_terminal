#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

std::optional<QString> quote_terminal_paths(
    const QStringList& paths,
    const QStringList& command);

std::optional<QString> terminal_drop_text_for_local_urls(
    const QList<QUrl>& urls,
    const QStringList& command);

void install_terminal_file_drop(
    VNM_TerminalSurface& surface,
    const QStringList& command);

} // namespace vnm_terminal::terminal_app
