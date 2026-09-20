#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QObject;

namespace vnm_terminal::terminal_app {

using Clipboard_completion = std::function<void(std::optional<QString>)>;
using Clipboard_cancel = std::function<void()>;

bool clipboard_broker_mode_requested(const QStringList& arguments);
int run_clipboard_text_broker(int argc, char** argv);

// Invoke the reader and cancellation on the GUI thread. Completion is deferred;
// cancellation and context destruction suppress it. Empty text succeeds,
// whereas nullopt reports failure.
Clipboard_cancel read_clipboard_text_with_broker(QObject* context, Clipboard_completion completion);

} // namespace vnm_terminal::terminal_app
