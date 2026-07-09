#include "terminal_settings_window.h"

#include "terminal_settings_controller.h"

#include "vnm_qml_chrome/vnm_qml_chrome_runtime.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QPoint>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QStringList>
#include <QUrl>
#include <QWindow>
#include <Qt>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace settings = vnm_terminal::terminal_app;

namespace {

QString component_error_string(const QQmlComponent& component)
{
    QStringList out;
    const auto errors = component.errors();
    for (const QQmlError& error : errors) {
        out.push_back(error.toString());
    }
    return out.join(QStringLiteral("\n"));
}

int clamp_axis(int value, int min_value, int max_value)
{
    if (max_value < min_value) {
        return min_value;
    }

    return qBound(min_value, value, max_value);
}

#ifdef Q_OS_WIN
struct Window_title_search
{
    const QString* title = nullptr;
    QRect geometry;
    HWND hwnd = nullptr;
};

BOOL CALLBACK find_visible_window_with_title(HWND hwnd, LPARAM user_data)
{
    auto* search = reinterpret_cast<Window_title_search*>(user_data);
    if (search == nullptr || search->title == nullptr || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    wchar_t title_buffer[256] = {};
    if (GetWindowTextW(hwnd, title_buffer, 256) <= 0) {
        return TRUE;
    }
    if (QString::fromWCharArray(title_buffer) != *search->title) {
        return TRUE;
    }

    RECT rect{};
    if (GetWindowRect(hwnd, &rect) &&
        rect.right > rect.left &&
        rect.bottom > rect.top)
    {
        search->hwnd = hwnd;
        search->geometry = QRect(
            QPoint(rect.left, rect.top),
            QPoint(rect.right - 1, rect.bottom - 1));
        return FALSE;
    }

    return TRUE;
}

QRect window_geometry_for_title(const QString& title)
{
    if (title.isEmpty()) {
        return {};
    }

    Window_title_search search;
    search.title = &title;
    EnumWindows(find_visible_window_with_title, reinterpret_cast<LPARAM>(&search));
    return search.geometry;
}

HWND window_handle_for_title(const QString& title)
{
    if (title.isEmpty()) {
        return nullptr;
    }

    Window_title_search search;
    search.title = &title;
    EnumWindows(find_visible_window_with_title, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

void show_window_above_anchor(QWindow& window, const QString& preferred_window_title)
{
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd == nullptr) {
        return;
    }

    const HWND owner = window_handle_for_title(preferred_window_title);
    if (owner != nullptr) {
        SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(
        hwnd,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
}
#endif

QRect fallback_anchor_geometry(const QString& preferred_window_title)
{
#ifdef Q_OS_WIN
    const QRect preferred_geometry = window_geometry_for_title(preferred_window_title);
    if (!preferred_geometry.isEmpty()) {
        return preferred_geometry;
    }

    const HWND foreground_window = GetForegroundWindow();
    RECT rect{};
    if (foreground_window != nullptr &&
        GetWindowRect(foreground_window, &rect) &&
        rect.right > rect.left &&
        rect.bottom > rect.top)
    {
        return QRect(
            QPoint(rect.left, rect.top),
            QPoint(rect.right - 1, rect.bottom - 1));
    }
#endif

    QScreen* screen = QGuiApplication::primaryScreen();
    return screen != nullptr ? screen->availableGeometry() : QRect{};
}

// A self-contained frameless QML Window styled with the shared VNM_Chrome
// titlebar so it visually belongs to the terminal. Its controls bind directly
// to the live surface (context property `surface`) for immediate apply; the
// `settings` controller supplies the monospace font list.
//
// The dialog styles every control itself (dialog-local inline `component`
// definitions over QtQuick.Controls.Basic, carrying the S_ prefix) because the
// stock Basic style is light-themed and clashes with the dark chrome. The
// palette is derived from the chrome colors of the main terminal window so the
// two windows read as one family. Only the color-scheme grid scrolls; the form
// below it is always fully visible.
constexpr const char* k_settings_window_qml = R"qml(
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Basic as Basic
import VNM_Chrome

Window {
    id: win
    objectName: "terminal_settings_window"

    width: 560
    height: 660
    minimumWidth: 480
    minimumHeight: 560
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint
    color: "#202020"
    title: "vnm_terminal - Settings"

    signal close_requested()
    signal move_requested()
    signal resize_requested(int edges)

    readonly property int titlebar_height: 32
    readonly property real frame_border_width: 1
    readonly property bool resize_enabled: visibility === Window.Windowed

    readonly property color section_color:      "#8d99a8"
    readonly property color label_color:        "#9aa4b2"
    readonly property color value_color:        "#dfe5ee"
    readonly property color hint_color:         "#6c7886"
    readonly property color warning_color:      "#d6b25a"
    readonly property color accent_color:       "#71b4ff"
    readonly property color separator_color:    "#343434"
    readonly property color field_color:        "#262626"
    readonly property color field_hover_color:  "#2e2e2e"
    readonly property color field_border_color: "#3a3a3a"
    readonly property color field_focus_color:  "#5a5a5a"
    readonly property color popup_color:        "#242424"
    readonly property color row_hover_color:    "#303030"
    readonly property color card_color:         "#252525"
    readonly property color card_hover_color:   "#2d2d2d"
    readonly property color card_border_color:  "#383838"

    component S_SectionHeader: RowLayout {
        property alias text: section_text.text

        Layout.fillWidth: true
        spacing: 10

        Text {
            id: section_text
            color: win.section_color
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
            font.capitalization: Font.AllUppercase
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: win.separator_color
        }
    }

    component S_Label: Text {
        color: win.label_color
        font.pixelSize: 12
    }

    component S_Hint: Text {
        color: win.hint_color
        font.pixelSize: 11
    }

    component S_ScrollBar: Basic.ScrollBar {
        id: bar

        implicitWidth: 8
        padding: 2
        policy: Basic.ScrollBar.AlwaysOn
        visible: bar.size < 1.0

        background: Rectangle {
            color: "transparent"
        }

        contentItem: Rectangle {
            implicitWidth: 4
            radius: 2
            color: bar.pressed ? "#5d6979" : "#414c5b"
        }
    }

    component S_Combo: Basic.ComboBox {
        id: combo

        // Per-row hooks for the popup list: `row_enabled` grays out rows that
        // are not currently selectable, and `use_row_font` renders each row in
        // the font family it names (for font pickers).
        property var  row_enabled: (row) => true
        property bool use_row_font: false

        implicitHeight: 30
        font.pixelSize: 12
        hoverEnabled: true

        background: Rectangle {
            radius: 4
            color: combo.hovered || combo.down ? win.field_hover_color : win.field_color
            border.width: 1
            border.color: combo.activeFocus || combo.down
                ? win.field_focus_color : win.field_border_color

            Behavior on color { ColorAnimation { duration: 100 } }
        }

        contentItem: Text {
            leftPadding: 10
            rightPadding: 24
            text: combo.displayText
            font: combo.font
            color: win.value_color
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        indicator: Text {
            x: combo.width - width - 9
            y: (combo.height - height) / 2
            text: "\u25BE"
            font.pixelSize: 10
            color: win.label_color
        }

        delegate: Basic.ItemDelegate {
            id: combo_row

            required property int index
            required property var modelData

            width: ListView.view ? ListView.view.width : 0
            height: 28
            enabled: combo.row_enabled(modelData)
            highlighted: combo.highlightedIndex === index
            hoverEnabled: true

            contentItem: Text {
                leftPadding: 4
                text: combo.textRole ? combo_row.modelData[combo.textRole] : combo_row.modelData
                font.pixelSize: 12
                font.family: combo.use_row_font ? combo_row.modelData : combo.font.family
                color: !combo_row.enabled ? win.hint_color
                    : combo_row.index === combo.currentIndex ? win.accent_color : win.value_color
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                radius: 3
                color: combo_row.highlighted ? win.row_hover_color : "transparent"
            }
        }

        popup: Basic.Popup {
            y: combo.height + 4
            width: combo.width
            padding: 6
            implicitHeight: Math.min(
                contentItem.implicitHeight + topPadding + bottomPadding, 280)

            background: Rectangle {
                radius: 6
                color: win.popup_color
                border.width: 1
                border.color: win.field_border_color
            }

            contentItem: ListView {
                implicitHeight: contentHeight
                clip: true
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                boundsBehavior: Flickable.StopAtBounds

                Basic.ScrollBar.vertical: S_ScrollBar {}
            }
        }
    }
)qml"
// MSVC C2026 caps one literal at ~16 KB; adjacent raw strings concatenate.
R"qml(
    component S_SpinBox: Basic.SpinBox {
        id: spin

        implicitWidth: 130
        implicitHeight: 30
        editable: true
        font.pixelSize: 12
        leftPadding: 32
        rightPadding: 32

        background: Rectangle {
            radius: 4
            color: win.field_color
            border.width: 1
            border.color: spin.activeFocus ? win.field_focus_color : win.field_border_color
        }

        contentItem: TextInput {
            text: spin.displayText
            font: spin.font
            color: win.value_color
            selectionColor: "#2f4a6b"
            selectedTextColor: win.value_color
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: Qt.ImhDigitsOnly
            clip: true
        }

        down.indicator: Rectangle {
            x: 2
            y: 2
            width: 26
            height: spin.height - 4
            radius: 3
            color: spin.down.pressed ? win.field_border_color
                : down_hover.hovered ? win.row_hover_color : "transparent"

            HoverHandler { id: down_hover }

            Text {
                anchors.centerIn: parent
                text: "\u2212"
                font.pixelSize: 13
                color: win.label_color
                opacity: spin.value > spin.from ? 1.0 : 0.35
            }
        }

        up.indicator: Rectangle {
            x: spin.width - width - 2
            y: 2
            width: 26
            height: spin.height - 4
            radius: 3
            color: spin.up.pressed ? win.field_border_color
                : up_hover.hovered ? win.row_hover_color : "transparent"

            HoverHandler { id: up_hover }

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 13
                color: win.label_color
                opacity: spin.value < spin.to ? 1.0 : 0.35
            }
        }
    }

    component S_Switch: Basic.Switch {
        id: sw

        implicitHeight: 30
        hoverEnabled: true

        indicator: Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 38
            implicitHeight: 20
            radius: height / 2
            color: sw.checked ? "#33598a" : "#222a35"
            border.width: 1
            border.color: sw.checked ? "#46719f" : "#36404e"

            Behavior on color { ColorAnimation { duration: 120 } }

            Rectangle {
                width: 14
                height: 14
                radius: 7
                anchors.verticalCenter: parent.verticalCenter
                x: sw.checked ? parent.width - width - 3 : 3
                color: sw.pressed ? "#c4d2e2"
                    : sw.hovered ? "#eef4fb" : "#dfe9f5"

                Behavior on x {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
            }
        }
    }

    component S_ProgressBar: Basic.ProgressBar {
        id: progress

        implicitHeight: 4

        background: Rectangle {
            radius: 2
            color: win.field_color
        }

        contentItem: Item {
            clip: true

            Rectangle {
                width: parent.width * progress.position
                height: parent.height
                radius: 2
                color: win.accent_color
                visible: !progress.indeterminate
            }

            Rectangle {
                id: progress_sweep
                width: parent.width * 0.3
                height: parent.height
                radius: 2
                color: win.accent_color
                visible: progress.indeterminate

                NumberAnimation on x {
                    running: progress.indeterminate && progress.visible
                    loops: Animation.Infinite
                    from: -progress_sweep.width
                    to: progress.width
                    duration: 1200
                }
            }
        }
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: win.close_requested()
    }

    VNM_ChromeTheme {
        id: settings_theme

        titlebar: "#202020"
        titlebar_text: "#e8e8e8"
        titlebar_button_icon: "#d8d8d8"
        titlebar_button_hover: "#303030"
        titlebar_button_pressed: "#3a3a3a"
        titlebar_close_hover: "#c6303a"
        titlebar_close_pressed: "#96222a"
        titlebar_content_border: "#343434"
        window_frame_border: "#343434"
    }

    Rectangle {
        anchors.fill: parent
        color: settings_theme.window_frame_border
        z: -100
    }

    Item {
        id: content
        anchors.fill: parent
        anchors.margins: win.resize_enabled ? win.frame_border_width : 0
        clip: true

        Rectangle {
            anchors.fill: parent
            color: win.color
        }

        VNM_ChromeSideResizeLayer {
            anchors.fill: parent
            resize_enabled: win.resize_enabled
            resize_border_width: 6

            onResize_requested: (edges) => win.resize_requested(edges)
        }

        VNM_ChromeBottomResizeLayer {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 6
            resize_enabled: win.resize_enabled
            resize_border_width: 6

            onResize_requested: (edges) => win.resize_requested(edges)
        }

        VNM_ChromeTitleBar {
            id: settings_titlebar
            objectName: "settings_window_titlebar"

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: win.titlebar_height
            theme: settings_theme
            title: win.title
            active: win.active
            resize_enabled: win.resize_enabled
            animated_mark_visible: false
            minimize_button_visible: false
            maximize_button_visible: false

            onMove_requested: win.move_requested()
            onResize_requested: (edges) => win.resize_requested(edges)
            onClose_requested: win.close_requested()
        }

        ColumnLayout {
            objectName: "settings_window_body"

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: settings_titlebar.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 16
            spacing: 10

            S_SectionHeader { text: "Color scheme" }

            GridView {
                id: scheme_grid
                objectName: "scheme_list"

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 96
                clip: true
                cellWidth: Math.floor(width / 2)
                cellHeight: 78
                model: surface.available_color_schemes()
                currentIndex: model.indexOf(surface.colorScheme)
                boundsBehavior: Flickable.StopAtBounds

                Basic.ScrollBar.vertical: S_ScrollBar {}

                Connections {
                    target: win
                    function onVisibleChanged() {
                        if (win.visible && scheme_grid.currentIndex >= 0) {
                            scheme_grid.positionViewAtIndex(
                                scheme_grid.currentIndex, GridView.Contain)
                        }
                    }
                }

                delegate: Item {
                    required property string modelData

                    readonly property var preview: surface.color_scheme_preview(modelData)
                    readonly property bool selected: modelData === surface.colorScheme

                    width: scheme_grid.cellWidth
                    height: scheme_grid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.rightMargin: 10
                        anchors.bottomMargin: 10
                        radius: 5
                        color: card_hover.hovered ? win.card_hover_color : win.card_color
                        border.width: 1
                        border.color: selected ? win.accent_color : win.card_border_color

                        Behavior on color { ColorAnimation { duration: 100 } }

                        HoverHandler {
                            id: card_hover
                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: surface.colorScheme = modelData
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 9
                            spacing: 6

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 3
                                color: preview.background
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.06)

                                Row {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Repeater {
                                        model: 16

                                        Rectangle {
                                            required property int index
                                            width: 7
                                            height: 10
                                            radius: 2
                                            color: preview.ansi[index]
                                        }
                                    }
                                }

                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "Aa"
                                    color: preview.foreground
                                    font.family: surface.fontFamily
                                    font.pixelSize: 12
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: selected ? win.value_color : win.label_color
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }

                                Text {
                                    visible: selected
                                    text: "\u2713"
                                    font.pixelSize: 12
                                    color: win.accent_color
                                }
                            }
                        }
                    }
                }
            }
)qml"
R"qml(
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 14
                rowSpacing: 10

                S_SectionHeader {
                    text: "Font"
                    Layout.columnSpan: 2
                    Layout.topMargin: 6
                }

                S_Label { text: "Family"; Layout.preferredWidth: 110 }

                S_Combo {
                    objectName: "font_family_combo"
                    Layout.fillWidth: true
                    use_row_font: true
                    model: settings.available_font_families()
                    currentIndex: Math.max(0, model.indexOf(surface.fontFamily))
                    onActivated: surface.fontFamily = currentText
                }

                S_Label { text: "Size" }

                S_SpinBox {
                    objectName: "font_size_spin"
                    from: 6
                    to: 72
                    value: Math.round(surface.fontSize)
                    onValueModified: surface.fontSize = value
                }

                S_SectionHeader {
                    text: "Rendering"
                    Layout.columnSpan: 2
                    Layout.topMargin: 10
                }

                S_Label { text: "Renderer" }

                S_Combo {
                    objectName: "renderer_mode_combo"
                    Layout.fillWidth: true
                    // "MSDF" maps to Auto (Text_renderer_mode 0): crisp,
                    // resolution-independent MSDF with automatic per-glyph
                    // glyph-atlas fallback. "Glyph" forces the bitmap atlas
                    // (2). The MSDF entry is disabled when MSDF cannot render
                    // the selected font, so the panel never offers a renderer
                    // that would silently do nothing.
                    textRole: "label"
                    model: [
                        { label: "MSDF",  is_glyph: false },
                        { label: "Glyph", is_glyph: true }
                    ]
                    row_enabled: (row) => row.is_glyph || surface.msdfTextAvailable
                    // Read the renderer actually in effect: when MSDF cannot
                    // render the font, the combo shows Glyph (the real
                    // fallback) instead of claiming MSDF. The mode preference
                    // is left untouched, so a capable font shows MSDF again.
                    currentIndex: (surface.textRendererMode === 2
                        || !surface.msdfTextAvailable) ? 1 : 0
                    onActivated: (index) =>
                        surface.textRendererMode = index === 1 ? 2 : 0
                }

                Item {}

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            objectName: "renderer_warning_glyph"
                            visible: !surface.msdfTextChecking
                                && !surface.msdfTextAvailable
                            text: "\u26A0"
                            font.pixelSize: 12
                            color: win.warning_color
                        }

                        Text {
                            objectName: "renderer_status_label"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                            color: (!surface.msdfTextChecking && !surface.msdfTextAvailable)
                                ? win.warning_color : win.hint_color
                            text: {
                                if (surface.msdfTextChecking)
                                    return "Checking whether MSDF can render this font..."
                                if (!surface.msdfTextAvailable)
                                    return "MSDF is not available for this font; rendering with the glyph atlas."
                                if (surface.textRendererMode === 2)
                                    return "Rendering: glyph atlas."
                                return "Rendering: MSDF."
                            }
                        }
                    }

                    S_ProgressBar {
                        Layout.fillWidth: true
                        indeterminate: true
                        visible: surface.msdfTextChecking
                    }
                }

                S_Label { text: "LCD subpixel" }

                S_Switch {
                    objectName: "lcd_subpixel_switch"
                    // lcdSubpixelOrder: AUTO=0 (on, auto-detected order), NONE=1
                    // (off, grayscale). RGB/BGR/etc. chosen via the CLI also
                    // read as on here.
                    checked: surface.lcdSubpixelOrder !== 1
                    onToggled: surface.lcdSubpixelOrder = checked ? 0 : 1
                }

                S_SectionHeader {
                    text: "Scrollback"
                    Layout.columnSpan: 2
                    Layout.topMargin: 10
                }

                S_Label { text: "Lines" }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    S_SpinBox {
                        objectName: "scrollback_spin"
                        implicitWidth: 150
                        from: 0
                        to: 1000000
                        stepSize: 1000
                        value: surface.scrollbackLimit
                        onValueModified: surface.scrollbackLimit = value
                    }

                    S_Hint {
                        Layout.fillWidth: true
                        text: "0 keeps no history beyond the screen."
                        elide: Text.ElideRight
                    }
                }

                S_SectionHeader {
                    text: "Behavior"
                    Layout.columnSpan: 2
                    Layout.topMargin: 10
                }

                S_Label { text: "Row timestamps" }

                S_Switch {
                    objectName: "row_timestamp_switch"
                    checked: surface.rowTimestampTooltipEnabled
                    onToggled: surface.rowTimestampTooltipEnabled = checked
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 2
                implicitHeight: 1
                color: win.separator_color
            }

            TextEdit {
                objectName: "build_provenance_text"

                Layout.fillWidth: true
                Layout.preferredHeight: contentHeight

                text: settings.buildProvenanceText
                textFormat: TextEdit.PlainText
                color: win.hint_color
                selectedTextColor: win.value_color
                selectionColor: "#2f4a6b"
                font.family: surface.fontFamily
                font.pixelSize: 10
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
            }
        }
    }
}
)qml";

} // namespace

settings::Terminal_settings_window::Terminal_settings_window(
    QQmlEngine&                   engine,
    VNM_TerminalSurface&          surface,
    Terminal_settings_controller& controller,
    QObject*                      parent)
:
    QObject(parent)
{
    if (!vnm_init_qml_chrome_runtime(engine)) {
        m_error_string = QStringLiteral("failed to initialize vnm_qml_chrome runtime");
        return;
    }

    auto* context = new QQmlContext(engine.rootContext(), this);
    context->setContextProperty(QStringLiteral("surface"), &surface);
    context->setContextProperty(QStringLiteral("settings"), &controller);

    QQmlComponent component(&engine);
    component.setData(
        k_settings_window_qml,
        QUrl(QStringLiteral("qrc:/vnm_terminal/terminal_settings_window.qml")));
    if (!component.isReady()) {
        m_error_string = component_error_string(component);
        return;
    }

    m_root_object.reset(component.create(context));
    if (m_root_object == nullptr) {
        m_error_string = component_error_string(component);
        return;
    }

    m_window = qobject_cast<QQuickWindow*>(m_root_object.get());
    if (m_window == nullptr) {
        m_error_string = QStringLiteral("settings window QML root is not a Window");
        m_root_object.reset();
        return;
    }

    QObject::connect(
        m_root_object.get(),
        SIGNAL(close_requested()),
        this,
        SLOT(handle_close_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(move_requested()),
        this,
        SLOT(handle_move_requested()));
    QObject::connect(
        m_root_object.get(),
        SIGNAL(resize_requested(int)),
        this,
        SLOT(handle_resize_requested(int)));
}

settings::Terminal_settings_window::~Terminal_settings_window() = default;

bool settings::Terminal_settings_window::is_valid() const
{
    return m_window != nullptr;
}

QString settings::Terminal_settings_window::error_string() const
{
    return m_error_string;
}

void settings::Terminal_settings_window::set_transient_parent(QWindow* parent)
{
    if (m_window != nullptr) {
        m_window->setTransientParent(parent);
    }
}

void settings::Terminal_settings_window::set_fallback_anchor_window_title(const QString& title)
{
    m_fallback_anchor_window_title = title;
}

void settings::Terminal_settings_window::show_window()
{
    if (m_window == nullptr) {
        return;
    }

    place_within_transient_parent();

    if (!m_window->isVisible()) {
        m_window->show();
    }

#ifdef Q_OS_WIN
    show_window_above_anchor(*m_window, m_fallback_anchor_window_title);
#endif
    m_window->raise();
    m_window->requestActivate();
}

void settings::Terminal_settings_window::place_within_transient_parent()
{
    if (m_window == nullptr) {
        return;
    }

    const QWindow* anchor = m_window->transientParent();
    const QRect anchor_geometry =
        anchor != nullptr && anchor->isVisible()
            ? anchor->geometry()
            : fallback_anchor_geometry(m_fallback_anchor_window_title);
    if (anchor_geometry.isEmpty()) {
        return;
    }

    const int width = qMax(1, m_window->width());
    const int height = qMax(1, m_window->height());
    QPoint top_left = m_positioned
        ? m_window->position()
        : anchor_geometry.center() - QPoint(width / 2, height / 2);

    top_left.setX(clamp_axis(
        top_left.x(),
        anchor_geometry.left(),
        anchor_geometry.left() + anchor_geometry.width() - width));
    top_left.setY(clamp_axis(
        top_left.y(),
        anchor_geometry.top(),
        anchor_geometry.top() + anchor_geometry.height() - height));

    m_window->setPosition(top_left);
    m_positioned = true;
}

void settings::Terminal_settings_window::handle_close_requested()
{
    if (m_window != nullptr) {
        m_window->hide();
    }
}

void settings::Terminal_settings_window::handle_move_requested()
{
    if (m_window != nullptr) {
        (void)m_window->startSystemMove();
    }
}

void settings::Terminal_settings_window::handle_resize_requested(int edges)
{
    if (m_window != nullptr) {
        (void)m_window->startSystemResize(Qt::Edges(QFlag(edges)));
    }
}
