#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace vnm_terminal::terminal_app {

bool clipboard_broker_mode_requested(const QStringList& arguments);
int run_clipboard_text_broker(int argc, char** argv);
std::optional<QString> read_clipboard_text_with_broker();

} // namespace vnm_terminal::terminal_app
