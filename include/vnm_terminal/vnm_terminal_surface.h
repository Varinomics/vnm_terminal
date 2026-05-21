#pragma once

#include <QQuickItem>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>

class QQuickWindow;
class QScreen;
class QHoverEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QInputMethodEvent;

namespace vnm_terminal::internal {
class Terminal_backend;
struct Terminal_backend_error;
struct Terminal_viewport_state;
struct Terminal_session_notification;
struct Terminal_session_result;
class VNM_TerminalSurface_render_bridge;
}

class VNM_TerminalSurface : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString fontFamily
        READ font_family WRITE set_font_family NOTIFY font_family_changed)
    Q_PROPERTY(qreal fontSize
        READ font_size WRITE set_font_size NOTIFY font_size_changed)
    Q_PROPERTY(QString colorTheme
        READ color_theme WRITE set_color_theme NOTIFY color_theme_changed)
    Q_PROPERTY(Cursor_style cursorStyle
        READ cursor_style WRITE set_cursor_style NOTIFY cursor_style_changed)
    Q_PROPERTY(bool cursorBlinkEnabled
        READ cursor_blink_enabled WRITE set_cursor_blink_enabled
        NOTIFY cursor_blink_enabled_changed)
    Q_PROPERTY(int scrollbackLimit
        READ scrollback_limit WRITE set_scrollback_limit NOTIFY scrollback_limit_changed)
    Q_PROPERTY(int synchronizedOutputStaleTimeoutMs
        READ synchronized_output_stale_timeout_ms
        WRITE set_synchronized_output_stale_timeout_ms
        NOTIFY synchronized_output_stale_timeout_ms_changed)
    Q_PROPERTY(Mouse_reporting_policy mouseReportingPolicy
        READ mouse_reporting_policy WRITE set_mouse_reporting_policy
        NOTIFY mouse_reporting_policy_changed)
    Q_PROPERTY(Copy_shortcut_policy copyShortcutPolicy
        READ copy_shortcut_policy WRITE set_copy_shortcut_policy
        NOTIFY copy_shortcut_policy_changed)
    Q_PROPERTY(Wheel_event_policy wheelEventPolicy
        READ wheel_event_policy WRITE set_wheel_event_policy
        NOTIFY wheel_event_policy_changed)
    Q_PROPERTY(Alternate_screen_wheel_policy alternateScreenWheelPolicy
        READ alternate_screen_wheel_policy WRITE set_alternate_screen_wheel_policy
        NOTIFY alternate_screen_wheel_policy_changed)
    Q_PROPERTY(Bracketed_paste_policy bracketedPastePolicy
        READ bracketed_paste_policy WRITE set_bracketed_paste_policy
        NOTIFY bracketed_paste_policy_changed)
    Q_PROPERTY(Bell_policy audibleBellPolicy
        READ audible_bell_policy WRITE set_audible_bell_policy
        NOTIFY audible_bell_policy_changed)
    Q_PROPERTY(Bell_policy visualBellPolicy
        READ visual_bell_policy WRITE set_visual_bell_policy
        NOTIFY visual_bell_policy_changed)
    Q_PROPERTY(QString terminalTitle READ terminal_title NOTIFY terminal_title_changed)
    Q_PROPERTY(QString terminalIconName
        READ terminal_icon_name NOTIFY terminal_icon_name_changed)
    Q_PROPERTY(Process_state processState READ process_state NOTIFY process_state_changed)
    Q_PROPERTY(bool backendReady READ backend_ready NOTIFY backend_ready_changed)
    Q_PROPERTY(bool backendGeometryInSync READ backend_geometry_in_sync NOTIFY geometry_sync_changed)
    Q_PROPERTY(int rows READ rows NOTIFY grid_geometry_changed)
    Q_PROPERTY(int columns READ columns NOTIFY grid_geometry_changed)
    Q_PROPERTY(int scrollbackRows READ scrollback_rows NOTIFY viewport_changed)
    Q_PROPERTY(int viewportVisibleRows READ viewport_visible_rows NOTIFY viewport_changed)
    Q_PROPERTY(int viewportOffsetFromTail
        READ viewport_offset_from_tail NOTIFY viewport_changed)
    Q_PROPERTY(bool viewportAtTail READ viewport_at_tail NOTIFY viewport_changed)
    Q_PROPERTY(Selection_state selectionState READ selection_state NOTIFY selection_changed)

public:
    enum class Cursor_style
    {
        BLOCK,
        BAR,
        UNDERLINE,
    };
    Q_ENUM(Cursor_style)

    enum class Mouse_reporting_policy
    {
        DISABLED,
        APPLICATION_CONTROLLED,
    };
    Q_ENUM(Mouse_reporting_policy)

    enum class Copy_shortcut_policy
    {
        TERMINAL_INPUT,
        COPY_SELECTION_OR_TERMINAL_INPUT,
        COPY_SELECTION_OR_IGNORE,
    };
    Q_ENUM(Copy_shortcut_policy)

    enum class Wheel_event_policy
    {
        APPLICATION_CONTROLLED,
        LOCAL_SCROLLBACK_FIRST,
        LOCAL_SCROLLBACK_ONLY,
    };
    Q_ENUM(Wheel_event_policy)

    enum class Alternate_screen_wheel_policy
    {
        MOUSE_REPORTING_FIRST,
        CURSOR_KEYS,
        PAGE_KEYS,
    };
    Q_ENUM(Alternate_screen_wheel_policy)

    enum class Bracketed_paste_policy
    {
        DISABLED,
        APPLICATION_CONTROLLED,
        ENABLED,
    };
    Q_ENUM(Bracketed_paste_policy)

    enum class Bell_policy
    {
        DISABLED,
        ENABLED,
    };
    Q_ENUM(Bell_policy)

    enum class Process_state
    {
        NOT_STARTED,
        STARTING,
        RUNNING,
        EXITED,
        FAILED,
    };
    Q_ENUM(Process_state)

    enum class Exit_reason
    {
        EXITED,
        INTERRUPTED,
        TERMINATED,
        FAILED_TO_START,
    };
    Q_ENUM(Exit_reason)

    enum class Backend_error_code
    {
        INVALID_LAUNCH_CONFIG,
        INVALID_INITIAL_GRID_SIZE,
        WORKING_DIRECTORY_UNAVAILABLE,
        START_FAILED,
        WRITE_FAILED,
        RESIZE_FAILED,
        INTERRUPT_FAILED,
        TERMINATE_FAILED,
        OUTPUT_OVERFLOW,
        CALLBACK_MISSING,
        READ_FAILED,
    };
    Q_ENUM(Backend_error_code)

    enum class Clipboard_response_decision
    {
        DENY,
        ALLOW,
    };
    Q_ENUM(Clipboard_response_decision)

    enum class Selection_state
    {
        NONE,
        ACTIVE,
    };
    Q_ENUM(Selection_state)

    explicit VNM_TerminalSurface(QQuickItem* parent = nullptr);
    ~VNM_TerminalSurface() override;

    QString font_family() const;
    void set_font_family(const QString& font_family);

    qreal font_size() const;
    void set_font_size(qreal font_size);

    QString color_theme() const;
    void set_color_theme(const QString& color_theme);

    Cursor_style cursor_style() const;
    void set_cursor_style(Cursor_style cursor_style);

    bool cursor_blink_enabled() const;
    void set_cursor_blink_enabled(bool enabled);

    int scrollback_limit() const;
    void set_scrollback_limit(int limit);

    QString backend_output_capture_path() const;
    void set_backend_output_capture_path(const QString& path);

    int synchronized_output_stale_timeout_ms() const;
    void set_synchronized_output_stale_timeout_ms(int timeout_ms);

    Mouse_reporting_policy mouse_reporting_policy() const;
    void set_mouse_reporting_policy(Mouse_reporting_policy policy);

    Copy_shortcut_policy copy_shortcut_policy() const;
    void set_copy_shortcut_policy(Copy_shortcut_policy policy);

    Wheel_event_policy wheel_event_policy() const;
    void set_wheel_event_policy(Wheel_event_policy policy);

    Alternate_screen_wheel_policy alternate_screen_wheel_policy() const;
    void set_alternate_screen_wheel_policy(Alternate_screen_wheel_policy policy);

    Bracketed_paste_policy bracketed_paste_policy() const;
    void set_bracketed_paste_policy(Bracketed_paste_policy policy);

    Bell_policy audible_bell_policy() const;
    void set_audible_bell_policy(Bell_policy policy);

    Bell_policy visual_bell_policy() const;
    void set_visual_bell_policy(Bell_policy policy);

    QString terminal_title() const;
    QString terminal_icon_name() const;
    Process_state process_state() const;
    bool backend_ready() const;
    bool backend_geometry_in_sync() const;
    int rows() const;
    int columns() const;
    int scrollback_rows() const;
    int viewport_visible_rows() const;
    int viewport_offset_from_tail() const;
    bool viewport_at_tail() const;
    Selection_state selection_state() const;

    Q_INVOKABLE bool respond_clipboard_write(
        quint64                        request_id,
        Clipboard_response_decision    decision);

    Q_INVOKABLE QString selected_text();
    Q_INVOKABLE void    clear_selection();
    Q_INVOKABLE bool    paste_text(QString text);
    // Scrolls only when the published viewport is primary-screen scrollback
    // and can be updated immediately. Returns false for no session, no-op
    // deltas/offsets, non-primary buffers, hidden synchronized output, and
    // boundary-clamped scrolls.
    Q_INVOKABLE bool scroll_viewport_lines(int line_delta);
    Q_INVOKABLE bool scroll_to_offset_from_tail(int offset_from_tail);
    Q_INVOKABLE bool start_process(QStringList argv, QString working_directory = {});
    Q_INVOKABLE bool interrupt_process();
    Q_INVOKABLE bool terminate_process();

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void font_family_changed();

    void font_size_changed();
    void color_theme_changed();
    void cursor_style_changed();
    void cursor_blink_enabled_changed();
    void scrollback_limit_changed();
    void synchronized_output_stale_timeout_ms_changed();
    void mouse_reporting_policy_changed();
    void copy_shortcut_policy_changed();
    void wheel_event_policy_changed();
    void alternate_screen_wheel_policy_changed();
    void bracketed_paste_policy_changed();
    void audible_bell_policy_changed();
    void visual_bell_policy_changed();
    void terminal_title_changed();
    void terminal_icon_name_changed();
    void process_state_changed();
    void backend_ready_changed();
    void grid_geometry_changed();
    void geometry_sync_changed();
    void viewport_changed();
    void selection_changed();

    void process_started();
    void process_exited(Exit_reason reason, int exit_code);
    void backend_error(Backend_error_code code, QString message);
    void output_activity();
    void output_backpressure_changed(bool active);
    void bell_requested();

    void text_area_resize_requested(
        int                    rows,
        int                    columns);

    void clipboard_write_requested(
        quint64                request_id,
        QString                target_selection,
        QByteArray             payload);

private:
    friend class vnm_terminal::internal::VNM_TerminalSurface_render_bridge;

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void releaseResources() override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

    void refresh_grid_metrics();

    void set_grid_size(
        int                    rows,
        int                    columns);

    void set_viewport_state(
        const vnm_terminal::internal::Terminal_viewport_state&
                               state);

    void set_process_state(
        Process_state          state);

    void set_backend_ready(
        bool                   ready);

    void set_backend_geometry_in_sync(
        bool                   in_sync);

    void bind_window_signals(
        QQuickWindow*          window);

    void bind_screen_signals(
        QScreen*               screen);

    void handle_scene_graph_invalidated(
        std::uint64_t          window_binding_generation);

    bool start_process_with_backend(
        std::unique_ptr<vnm_terminal::internal::Terminal_backend>
                               backend,
        QStringList            argv,
        QString                working_directory);

    void drain_backend_callback_events();
    void refresh_active_session_geometry();
    void sync_from_session();
    void sync_synchronized_output_recovery_timer();
    void handle_synchronized_output_recovery_timeout();

    void replay_session_notification(
        const vnm_terminal::internal::Terminal_session_notification&
                               notification);

    void report_backend_error(
        vnm_terminal::internal::Terminal_backend_error
                               error,
        quint64                sequence = 0);

    void report_result_failure(
        const vnm_terminal::internal::Terminal_session_result&
                               result);

    void reset_session();
    bool copy_selected_text_to_clipboard();
    void set_selection_state(Selection_state state);

    QString                  m_font_family;
    qreal                    m_font_size                            = 13.0;
    QString                  m_color_theme                          = QStringLiteral("default");
    Cursor_style             m_cursor_style                         = Cursor_style::BLOCK;
    bool                     m_cursor_blink_enabled                 = true;
    int                      m_scrollback_limit                     = 10000;
    QString                  m_backend_output_capture_path;
    int                      m_synchronized_output_stale_timeout_ms = 1000;
    Mouse_reporting_policy   m_mouse_reporting_policy =
        Mouse_reporting_policy::APPLICATION_CONTROLLED;
    Copy_shortcut_policy     m_copy_shortcut_policy =
        Copy_shortcut_policy::COPY_SELECTION_OR_TERMINAL_INPUT;
    Wheel_event_policy       m_wheel_event_policy =
        Wheel_event_policy::APPLICATION_CONTROLLED;
    Alternate_screen_wheel_policy m_alternate_screen_wheel_policy =
        Alternate_screen_wheel_policy::MOUSE_REPORTING_FIRST;
    Bracketed_paste_policy   m_bracketed_paste_policy =
        Bracketed_paste_policy::APPLICATION_CONTROLLED;
    Bell_policy              m_audible_bell_policy       = Bell_policy::ENABLED;
    Bell_policy              m_visual_bell_policy        = Bell_policy::ENABLED;
    QString                  m_terminal_title;
    QString                  m_terminal_icon_name;
    Process_state            m_process_state             = Process_state::NOT_STARTED;
    bool                     m_backend_ready             = false;
    bool                     m_backend_geometry_in_sync  = false;
    int                      m_rows                      = 0;
    int                      m_columns                   = 0;
    int                      m_scrollback_rows           = 0;
    int                      m_viewport_visible_rows     = 0;
    int                      m_viewport_offset_from_tail = 0;
    bool                     m_viewport_at_tail          = true;
    Selection_state          m_selection_state           = Selection_state::NONE;

    struct Private;
    std::unique_ptr<Private> m_private;
};
