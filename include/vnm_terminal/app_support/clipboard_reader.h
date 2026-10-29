#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QObject;
class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

using Clipboard_completion = std::function<void(std::optional<QString>)>;
using Clipboard_cancel = std::function<void()>;
using Clipboard_image_only_handler = std::function<void()>;

bool clipboard_broker_mode_requested(const QStringList& arguments);
int run_clipboard_text_broker(int argc, char** argv);

// Invoke the reader and cancellation on the GUI thread. Completion is deferred;
// cancellation and context destruction suppress it. Empty text succeeds,
// whereas nullopt reports failure. When the clipboard holds an image and no
// text format, `image_only` runs right after the empty-text completion; a file
// reference beside the image does not count as text. Only the brokered Windows
// reader reports this. An empty broker_program relaunches the current executable,
// which must dispatch clipboard_broker_mode_requested before normal startup.
// Hosted workers supply the separately deployed broker instead.
Clipboard_cancel read_clipboard_text_with_broker(
    QObject*                      context,
    Clipboard_completion          completion,
    Clipboard_image_only_handler  image_only = {},
    QString                       broker_program = {});

// Windows hosts deploy vnm_terminal_clipboard_broker beside the worker host,
// or pass the program path of their separately named broker.
// Installs its cancellable reader and image-only Alt+V delivery on this surface.
// Other platforms retain the surface's ordinary Qt clipboard path.
void install_hosted_clipboard_reader(VNM_TerminalSurface& surface, QString broker_program = {});

} // namespace vnm_terminal::terminal_app
