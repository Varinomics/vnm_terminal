#include "vnm_terminal/app_support/clipboard_reader.h"
#include "process_arguments.h"

#include <QByteArray>

#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv)
{
    const auto arguments = vnm_terminal::terminal_app::process_arguments(argc, argv);
    if (!arguments.error.isEmpty()) {
        std::fprintf(stderr, "%s\n", arguments.error.toUtf8().constData());
        return EXIT_FAILURE;
    }
    if (!vnm_terminal::terminal_app::clipboard_broker_mode_requested(arguments.arguments)) {
        return EXIT_FAILURE;
    }
    // This executable has no application or hosted-worker startup path.
    return vnm_terminal::terminal_app::run_clipboard_text_broker(argc, argv);
}
