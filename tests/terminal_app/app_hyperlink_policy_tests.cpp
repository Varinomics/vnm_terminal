#include "app_hyperlink_policy.h"

#include "helpers/test_check.h"

#include <QByteArray>
#include <QUrl>

#include <array>
#include <optional>
#include <string>

namespace chrome = vnm_terminal::terminal_app;

namespace {

using vnm_terminal::test_helpers::check;

struct Hyperlink_policy_case
{
    const char* target;
    bool        accepted = false;
};

bool test_supported_scheme_policy()
{
    static constexpr std::array<Hyperlink_policy_case, 15> k_cases = {
        Hyperlink_policy_case{"https://example.test/docs?q=terminal#osc8", true},
        Hyperlink_policy_case{"HTTP://example.test/", true},
        Hyperlink_policy_case{"mailto:terminal@example.test", true},
        Hyperlink_policy_case{"https://", false},
        Hyperlink_policy_case{"https:relative-path", false},
        Hyperlink_policy_case{"mailto:", false},
        Hyperlink_policy_case{"mailto://example.test/address", false},
        Hyperlink_policy_case{"file:///C:/Windows/System32/calc.exe", false},
        Hyperlink_policy_case{"javascript:alert(1)", false},
        Hyperlink_policy_case{"data:text/html,terminal", false},
        Hyperlink_policy_case{"ssh://example.test/", false},
        Hyperlink_policy_case{"custom-handler:payload", false},
        Hyperlink_policy_case{"//example.test/path", false},
        Hyperlink_policy_case{"not-a-url", false},
        Hyperlink_policy_case{"", false},
    };

    bool ok = true;
    for (const Hyperlink_policy_case& test_case : k_cases) {
        const std::optional<QUrl> url = chrome::validated_terminal_hyperlink_url(
            QByteArray(test_case.target));
        const std::string message =
            std::string("hyperlink scheme policy for ") + test_case.target;
        ok &= check(
            url.has_value() == test_case.accepted,
            message);
    }
    return ok;
}

bool test_encoded_target_is_preserved()
{
    const QByteArray target = QByteArrayLiteral(
        "https://example.test/a%20path?source=terminal%20surface");
    const std::optional<QUrl> url = chrome::validated_terminal_hyperlink_url(target);

    bool ok = true;
    ok &= check(url.has_value(), "valid encoded hyperlink target is accepted");
    if (url.has_value()) {
        ok &= check(
            url->toEncoded() == target,
            "validated hyperlink preserves the encoded target for dispatch");
    }
    return ok;
}

}

int main()
{
    bool ok = true;
    ok &= test_supported_scheme_policy();
    ok &= test_encoded_target_is_preserved();
    return ok ? 0 : 1;
}
