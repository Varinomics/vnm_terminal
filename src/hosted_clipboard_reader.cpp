#include "vnm_terminal/app_support/clipboard_reader.h"

#include "vnm_terminal/app_support/app_shortcuts.h"
#include "vnm_terminal/vnm_terminal_surface.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>

#include <utility>

namespace vnm_terminal::terminal_app {

void install_hosted_clipboard_reader(VNM_TerminalSurface& surface, QString broker_program)
{
#if defined(Q_OS_WIN)
    if (broker_program.isEmpty()) {
        broker_program = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("vnm_terminal_clipboard_broker.exe"));
    }
    surface.set_clipboard_text_reader(
        [broker_program](
            QObject* context,
            Clipboard_completion completion)
        {
            const QPointer<QObject> target(context);
            return read_clipboard_text_with_broker(
                context,
                std::move(completion),
                [target] {
                    if (target) {
                        send_image_paste_shortcut(*target);
                    }
                },
                broker_program);
        });
#else
    static_cast<void>(surface);
    static_cast<void>(broker_program);
#endif
}

} // namespace vnm_terminal::terminal_app
