#pragma once

#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

namespace vnm_terminal::terminal_app {

class Codex_command_environment
{
public:
    bool prepare(QStringList& command, QProcessEnvironment& environment, QString& error);

private:
    QTemporaryDir m_directory;
};

} // namespace vnm_terminal::terminal_app
