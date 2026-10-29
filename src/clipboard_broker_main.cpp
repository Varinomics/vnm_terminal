#include "vnm_terminal/app_support/clipboard_reader.h"

#include <QStringList>

#include <cstdlib>

int main(int argc, char** argv)
{
    QStringList arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }
    if (!vnm_terminal::terminal_app::clipboard_broker_mode_requested(arguments)) {
        return EXIT_FAILURE;
    }
    // This executable has no application or hosted-worker startup path.
    return vnm_terminal::terminal_app::run_clipboard_text_broker(argc, argv);
}
