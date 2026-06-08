#include "app_clipboard_policy.h"

#include "app_common.h"

#include <QString>

namespace vnm_terminal::terminal_app {

void handle_clipboard_write_request(
    VNM_TerminalSurface&     surface,
    quint64                  request_id,
    const QString&           target_selection,
    qsizetype                payload_size,
    Osc52_clipboard_policy   policy)
{
    using Decision = VNM_TerminalSurface::Clipboard_response_decision;

    const bool target_is_clipboard =
        target_selection == QStringLiteral("c") ||
        target_selection == QStringLiteral("clipboard");
    const bool over_payload_cap = payload_size > k_osc52_clipboard_max_payload_bytes;
    const bool allow =
        policy == Osc52_clipboard_policy::ALLOW &&
        target_is_clipboard                     &&
        !over_payload_cap;

    if (!surface.respond_clipboard_write(
            request_id, allow ? Decision::ALLOW : Decision::DENY))
    {
        print_error(QStringLiteral("OSC 52 clipboard write response could not be delivered"));
        return;
    }

    // Surface only the surprising case: the session is trusted and the target is valid,
    // but the write is rejected solely because the payload exceeds the size cap. The
    // expected default-deny path stays quiet so a noisy program cannot flood stderr.
    if (policy == Osc52_clipboard_policy::ALLOW && target_is_clipboard && over_payload_cap) {
        print_error(QStringLiteral(
            "OSC 52 clipboard write denied: payload of %1 bytes exceeds the %2-byte limit")
            .arg(static_cast<qlonglong>(payload_size))
            .arg(k_osc52_clipboard_max_payload_bytes));
    }
}

} // namespace vnm_terminal::terminal_app
