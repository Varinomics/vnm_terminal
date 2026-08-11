#pragma once

#include <QByteArray>
#include <QUrl>

#include <optional>

namespace vnm_terminal::terminal_app {

/**
 * Validates an untrusted OSC 8 target for external dispatch by the app.
 *
 * The standalone app supports absolute HTTP, HTTPS, and mailto URLs. A
 * rejected target must not be passed to QDesktopServices or another handler.
 */
std::optional<QUrl> validated_terminal_hyperlink_url(const QByteArray& target);

} // namespace vnm_terminal::terminal_app
