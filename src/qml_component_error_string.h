#pragma once

class QQmlComponent;
class QString;

namespace vnm_terminal::terminal_app::detail {

QString qml_component_error_string(const QQmlComponent& component);

} // namespace vnm_terminal::terminal_app::detail
