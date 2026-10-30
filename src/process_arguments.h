#pragma once

#include <QString>
#include <QStringList>

namespace vnm_terminal::terminal_app {

struct Process_arguments
{
    QStringList arguments;
    QString     error;
};

Process_arguments process_arguments(int argc, char** argv);

#if defined(_WIN32)
// Parse a full Windows command line, including its executable-name argument.
Process_arguments windows_command_line_arguments(const wchar_t* command_line);
#endif

} // namespace vnm_terminal::terminal_app
