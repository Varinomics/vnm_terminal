#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>
#include <Qt>

#include <memory>

class QQmlContext;
class QQmlEngine;
class QQuickItem;
class QQuickWindow;
class VNM_TerminalSurface;

namespace vnm_terminal::terminal_app {

class Terminal_search_bar final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString resultText READ result_text NOTIFY result_text_changed)
    Q_PROPERTY(bool chromeActive READ chrome_active NOTIFY chrome_palette_changed)
    Q_PROPERTY(QColor chromeBackgroundColor
        READ chrome_background_color NOTIFY chrome_palette_changed)
    Q_PROPERTY(QColor chromeFrameEdgeColor
        READ chrome_frame_edge_color NOTIFY chrome_palette_changed)
    // The family the navigation chevrons are drawn in, as the font database
    // resolved it. The marked name is decided by vnm_fonts at registration and
    // is fixed for the life of the bar, so the QML reads it rather than naming
    // a face of its own.
    Q_PROPERTY(QString iconFontFamily READ icon_font_family CONSTANT)

public:
    Terminal_search_bar(
        QQmlEngine&          engine,
        QQuickWindow&        window,
        VNM_TerminalSurface& surface);
    ~Terminal_search_bar() override;

    bool    is_valid() const;
    bool    is_visible() const;
    QString error_string() const;
    QString result_text() const;
    QString icon_font_family() const;
    bool    chrome_active() const;
    QColor  chrome_background_color() const;
    QColor  chrome_frame_edge_color() const;
    QQuickItem* root_item() const;

    void set_text_font_family(const QString& font_family);
    bool focus_query();
    void commit_text(const QString& text);
    void send_key_press(
        int                   key,
        Qt::KeyboardModifiers modifiers,
        const QString&        text = {});
    void send_key_release(int key, Qt::KeyboardModifiers modifiers);
    void show_search();
    Q_INVOKABLE void dismiss_search();

signals:
    void visibility_changed(bool visible);
    void result_text_changed();
    void chrome_palette_changed();

private:
    QPointer<QQuickWindow>        m_window;
    QPointer<VNM_TerminalSurface> m_surface;
    std::unique_ptr<QQmlContext>   m_context;
    std::unique_ptr<QObject>       m_root_object;
    QQuickItem*                    m_root_item = nullptr;
    QPointer<QQuickItem>           m_query_item;
    QPointer<QQuickItem>           m_result_item;
    QString                        m_icon_font_family;
    QString                        m_error_string;
};

} // namespace vnm_terminal::terminal_app
