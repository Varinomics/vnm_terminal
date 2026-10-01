#include "vnm_terminal/app_support/terminal_settings_dialog.h"

#include "vnm_terminal/app_support/terminal_settings_controller.h"
#include "vnm_terminal/app_support/terminal_settings_model.h"
#include "vnm_terminal/app_support/terminal_settings_window.h"

#include <QQmlApplicationEngine>
#include <QWindow>

#include <utility>

namespace vnm_terminal::terminal_app {

Terminal_settings_dialog::Terminal_settings_dialog(
    QQmlApplicationEngine* engine, QString anchor_window_title, QObject* parent)
:
    QObject(parent),
    m_engine(engine),
    m_anchor_window_title(std::move(anchor_window_title))
{
    if (engine) {
        connect(engine, &QObject::destroyed, this, [this] {
            m_window.reset();
            m_model.reset();
        });
    }
}

Terminal_settings_dialog::~Terminal_settings_dialog() = default;

bool Terminal_settings_dialog::show_window(const QVariantMap& values, bool dark_mode)
{
    if (!m_engine) {
        m_error_string = QStringLiteral("Terminal settings require the application QML engine.");
        return false;
    }
    if (!m_window) {
        m_model = std::make_unique<Terminal_settings_model>();
        m_model->set_values(values);
        connect(m_model.get(), &Terminal_settings_model::changes_requested,
            this, &Terminal_settings_dialog::changes_requested);
        auto* controller = new Terminal_settings_controller(m_model.get());
        m_window = std::make_unique<Terminal_settings_window>(*m_engine, *m_model, *controller);
        if (!m_window->is_valid()) {
            m_error_string = m_window->error_string();
            m_window.reset();
            m_model.reset();
            return false;
        }
        m_window->set_fallback_anchor_window_title(m_anchor_window_title);
    }
    refresh(values, dark_mode);
    QWindow* anchor = nullptr;
    for (QObject* root : m_engine->rootObjects()) {
        auto* const window = qobject_cast<QWindow*>(root);
        if (window && window->isVisible()) {
            anchor = window;
            break;
        }
    }
    m_window->set_transient_parent(anchor);
    m_window->show_window();
    m_error_string.clear();
    return true;
}

void Terminal_settings_dialog::refresh(const QVariantMap& values, bool dark_mode)
{
    if (m_window) {
        m_model->set_values(values);
        m_window->set_dark_mode(dark_mode);
    }
}

QString Terminal_settings_dialog::error_string() const
{
    return m_error_string;
}

} // namespace vnm_terminal::terminal_app
