#include "vnm_terminal/app_support/terminal_display_settings.h"

#include "vnm_terminal/internal/terminal_color_scheme.h"

#include <QColor>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMetaType>
#include <QSettings>

#include <cmath>
#include <type_traits>
#include <utility>

namespace vnm_terminal::terminal_app {

namespace {

bool settings_values_valid(const QVariantMap& changes, bool snapshot)
{
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const QString& key    = it.key();
        const QVariant& value = it.value();
        if (key == QStringLiteral("color_scheme")) {
            if (value.metaType().id() != QMetaType::QString ||
                value.toString().contains(QChar(u'\0')) ||
                (snapshot ? value.toString().trimmed().isEmpty()
                          : !vnm_terminal::internal::find_color_scheme(value.toString())))
            {
                return false;
            }
        }
        else
        if (key == QStringLiteral("font_family")) {
            if (value.metaType().id() != QMetaType::QString ||
                value.toString().contains(QChar(u'\0')) ||
                (!snapshot && value.toString().trimmed().isEmpty()))
            {
                return false;
            }
        }
        else
        if (key == QStringLiteral("row_timestamp_tooltip_enabled")) {
            if (value.metaType().id() != QMetaType::Bool) {
                return false;
            }
        }
        else {
            double minimum = 0.0;
            double maximum = 0.0;
            if (key == QStringLiteral("font_size")) {
                minimum = 6.0;
                maximum = 72.0;
            }
            else
            if (key == QStringLiteral("text_renderer_mode")) {
                maximum = static_cast<int>(VNM_TerminalSurface::Text_renderer_mode::GLYPH);
            }
            else
            if (key == QStringLiteral("lcd_subpixel_order")) {
                maximum = static_cast<int>(VNM_TerminalSurface::Lcd_subpixel_order::VBGR);
            }
            else
            if (key == QStringLiteral("scrollback_limit")) {
                maximum = 1'000'000;
            }
            else {
                return false;
            }
            const int type      = value.metaType().id();
            const double number = value.toDouble();
            if ((type != QMetaType::Int && type != QMetaType::LongLong && type != QMetaType::Double) ||
                !std::isfinite(number) || number < minimum || number > maximum ||
                (key != QStringLiteral("font_size") && std::floor(number) != number))
            {
                return false;
            }
        }
    }
    return true;
}

Terminal_settings_snapshot settings_snapshot(
    const QVariantMap& values, Terminal_settings_snapshot base)
{
    const auto read = [
            &values
        ](
            const char* key,
            auto& target)
        {
            const auto it = values.constFind(QLatin1String(key));
            if (it != values.cend()) {
                target = it->value<std::remove_cvref_t<decltype(target)>>();
            }
        };
    read("color_scheme",                  base.color_scheme);
    read("font_family",                   base.font_family);
    read("font_size",                     base.font_size);
    read("text_renderer_mode",            base.text_renderer_mode);
    read("lcd_subpixel_order",            base.lcd_subpixel_order);
    read("row_timestamp_tooltip_enabled",  base.row_timestamp_tooltip_enabled);
    if (values.contains(QStringLiteral("scrollback_limit"))) {
        base.scrollback_limit = values.value(QStringLiteral("scrollback_limit")).toInt();
    }
    return base;
}

} // namespace

bool terminal_settings_changes_valid(const QVariantMap& changes)
{
    return settings_values_valid(changes, false);
}

std::optional<Terminal_settings_snapshot> terminal_settings_from_json(
    const QJsonValue& value, Terminal_settings_snapshot base)
{
    if (value.isUndefined()) {
        return base;
    }
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QVariantMap values = value.toObject().toVariantMap();
    if (!settings_values_valid(values, true)) {
        return std::nullopt;
    }
    return settings_snapshot(values, std::move(base));
}

QVariantMap terminal_settings_payload(const Terminal_settings_snapshot& settings)
{
    QVariantMap result{
        {QStringLiteral("color_scheme"), settings.color_scheme},
        {QStringLiteral("font_family"), settings.font_family},
        {QStringLiteral("font_size"), settings.font_size},
        {QStringLiteral("text_renderer_mode"), settings.text_renderer_mode},
        {QStringLiteral("lcd_subpixel_order"), settings.lcd_subpixel_order},
        {QStringLiteral("row_timestamp_tooltip_enabled"), settings.row_timestamp_tooltip_enabled},
    };
    if (settings.scrollback_limit) {
        result.insert(QStringLiteral("scrollback_limit"), *settings.scrollback_limit);
    }
    return result;
}

Terminal_display_settings::Terminal_display_settings(bool dark_mode, QSettings* store)
:
    m_store(store),
    m_values(terminal_settings_payload(
        store ? load_terminal_settings_snapshot(*store) : Terminal_settings_snapshot{})),
    m_dark_mode(dark_mode)
{
    if (store) {
        // The unscoped palette predates mode binding. Its background identifies
        // which mode can inherit it without turning a light terminal dark.
        const auto* previous = vnm_terminal::internal::find_color_scheme(
            m_values.value(QStringLiteral("color_scheme")).toString());
        if (previous) {
            const bool previous_dark = QColor::fromRgba(previous->background_rgba).lightnessF() < 0.5;
            (previous_dark ? m_dark_scheme : m_light_scheme) = previous->name;
        }
        const auto load_scheme = [
                store
            ](
                const QString& key,
                QString& scheme)
            {
                const auto* found = vnm_terminal::internal::find_color_scheme(store->value(key).toString());
                if (found) {
                    scheme = found->name;
                }
            };
        load_scheme(QStringLiteral("appearance/color_scheme_dark"), m_dark_scheme);
        load_scheme(QStringLiteral("appearance/color_scheme_light"), m_light_scheme);
    }
    m_values.insert(QStringLiteral("color_scheme"), m_dark_mode ? m_dark_scheme : m_light_scheme);
}

const QVariantMap& Terminal_display_settings::values() const
{
    return m_values;
}

bool Terminal_display_settings::set_dark_mode(bool dark_mode)
{
    if (m_dark_mode == dark_mode) {
        return false;
    }
    m_dark_mode = dark_mode;
    m_values.insert(QStringLiteral("color_scheme"), dark_mode ? m_dark_scheme : m_light_scheme);
    return true;
}

bool Terminal_display_settings::apply_changes(const QVariantMap& changes, bool source_dark_mode)
{
    if (!terminal_settings_changes_valid(changes)) {
        return false;
    }
    const QVariantMap previous    = m_values;
    const QString previous_dark  = m_dark_scheme;
    const QString previous_light = m_light_scheme;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.key() == QStringLiteral("color_scheme")) {
            (source_dark_mode ? m_dark_scheme : m_light_scheme) = it.value().toString();
        }
        else {
            m_values.insert(it.key(), it.value());
        }
    }
    m_values.insert(QStringLiteral("color_scheme"), m_dark_mode ? m_dark_scheme : m_light_scheme);
    if (previous == m_values && previous_dark == m_dark_scheme && previous_light == m_light_scheme) {
        return false;
    }
    if (m_store) {
        m_store->setValue(QStringLiteral("appearance/color_scheme_dark"), m_dark_scheme);
        m_store->setValue(QStringLiteral("appearance/color_scheme_light"), m_light_scheme);
        save_terminal_settings_snapshot(*m_store, settings_snapshot(m_values, {}));
    }
    return true;
}

} // namespace vnm_terminal::terminal_app
