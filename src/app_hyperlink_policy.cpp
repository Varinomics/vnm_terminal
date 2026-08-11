#include "app_hyperlink_policy.h"

#include <QString>

namespace vnm_terminal::terminal_app {

std::optional<QUrl> validated_terminal_hyperlink_url(const QByteArray& target)
{
    if (target.isEmpty()) {
        return std::nullopt;
    }

    const QUrl url = QUrl::fromEncoded(target, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.scheme().isEmpty()) {
        return std::nullopt;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) {
        return url.host().isEmpty() ? std::nullopt : std::optional<QUrl>{url};
    }

    if (scheme == QStringLiteral("mailto")) {
        return url.host().isEmpty() && !url.path().isEmpty()
            ? std::optional<QUrl>{url}
            : std::nullopt;
    }

    return std::nullopt;
}

} // namespace vnm_terminal::terminal_app
