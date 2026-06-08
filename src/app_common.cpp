#include "app_common.h"

#include <QByteArray>

#include <iostream>

namespace vnm_terminal::terminal_app {

void print_error(const QString& message)
{
    const QByteArray bytes = message.toUtf8();
    std::cerr << "vnm_terminal: " << bytes.constData() << '\n';
}

} // namespace vnm_terminal::terminal_app
