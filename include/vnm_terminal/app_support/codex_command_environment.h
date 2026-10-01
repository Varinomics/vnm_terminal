#pragma once

#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

namespace vnm_terminal::terminal_app {

class Codex_command_environment
{
public:
    // Keep this owner alive for the terminal session: its PATH commands live in
    // a private directory that is removed when the owner is destroyed.
    bool prepare(QStringList& command, QProcessEnvironment& environment, QString& error);

private:
    QTemporaryDir m_directory;
};

} // namespace vnm_terminal::terminal_app
