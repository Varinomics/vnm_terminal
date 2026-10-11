import QtQuick
import QtQuick.Window
import VNM_Chrome

Item {
    id: root
    objectName: "terminal_qml_chrome_root"

    property string title: ""
    property bool title_editing_enabled: true
    property bool title_reset_available: false
    property string activity_marker_text: ""
    property bool active: true
    property bool maximized: false
    property bool resize_enabled: true
    property bool fullscreen: false
    property bool wheel_delivery_indicator_visible: false
    property bool settings_button_visible: true
    property Component trailing_action_component: null
    property bool row_timestamp_tooltip_visible: false
    property rect row_timestamp_tooltip_row_rect: Qt.rect(0, 0, 0, 0)
    property bool row_timestamp_tooltip_scrollbar_visible: false
    property date row_timestamp_tooltip_timestamp: new Date()
    // Set from Terminal_chrome_palette when the component is created. The
    // colors are deliberately not spelled out here: C++ owns them so the
    // window clear color and the search bar cannot drift from the chrome.
    property color focused_frame_background_color
    property color unfocused_frame_background_color
    property color focused_frame_edge_color
    property color unfocused_frame_edge_color
    property color focused_titlebar_text_color
    property color unfocused_titlebar_text_color
    property color titlebar_button_icon_color:
        active ? "#e2e8f0" : "#8e97a3"
    property color titlebar_button_hover_color: "#272f3a"
    property color titlebar_button_pressed_color: "#343d4a"
    property color titlebar_close_hover_color: "#c6303a"
    property color titlebar_close_pressed_color: "#96222a"
    // The window ratio is the live one. Screen.devicePixelRatio never
    // re-notifies while the window stays on one screen, and a Windows scale
    // change updates that screen in place, so binding to it would pin the whole
    // chrome to the scale that was in force when the window opened.
    property real device_pixel_ratio:
        Window.window ? Window.window.devicePixelRatio : Screen.devicePixelRatio
    readonly property real base_resize_border_width: 6
    readonly property real resize_border_physical_reduction: 2
    readonly property real reduced_resize_border_width:
        reduced_chrome_width(
            base_resize_border_width,
            resize_border_physical_reduction)
    readonly property real base_titlebar_height: frame_shell.titlebar_item.implicitHeight
    readonly property real titlebar_physical_reduction: 2
    readonly property real reduced_titlebar_height:
        reduced_chrome_width(
            base_titlebar_height,
            titlebar_physical_reduction)
    property real titlebar_height: reduced_titlebar_height
    readonly property real frame_edge_thickness:
        1 / VNM_chrome_geometry.normalized_device_pixel_ratio(device_pixel_ratio)
    readonly property real active_frame_gap:
        Math.max(0, reduced_resize_border_width - frame_edge_thickness)
    readonly property real shell_outer_edge_thickness:
        fullscreen ? 0 : frame_edge_thickness
    readonly property real shell_inner_edge_thickness: frame_edge_thickness
    readonly property real shell_frame_gap: resize_enabled ? active_frame_gap : 0
    readonly property bool maximized_frame_overscan:
        maximized && !fullscreen && shell_outer_edge_thickness > 0
    readonly property color frame_edge_color:
        active ? focused_frame_edge_color : unfocused_frame_edge_color
    readonly property color frame_background_color:
        active ? focused_frame_background_color : unfocused_frame_background_color
    readonly property rect content_interior_rect: Qt.rect(
        content_interior_x,
        content_interior_y,
        content_interior_width,
        content_interior_height)
    readonly property real content_interior_x:
        frame_shell.x + frame_shell.content_interior_x
    readonly property real content_interior_y:
        frame_shell.y + frame_shell.content_interior_y
    readonly property real content_interior_width: frame_shell.content_interior_width
    readonly property real content_interior_height: frame_shell.content_interior_height
    readonly property Item titlebar_item: frame_shell.titlebar_item

    function reduced_chrome_width(logical_width, physical_reduction) {
        var dpr = VNM_chrome_geometry.normalized_device_pixel_ratio(device_pixel_ratio)
        return Math.max(
            0,
            VNM_chrome_geometry.snapped_logical_edge(logical_width, dpr)
                - physical_reduction / dpr)
    }

    signal move_requested()
    signal resize_requested(int edges)
    signal minimize_requested()
    signal maximize_toggle_requested()
    signal close_requested()
    signal settings_requested()
    signal title_edit_accepted(string title)
    signal title_reset_requested()

    VNM_ChromeTheme {
        id: terminal_chrome_theme

        titlebar: root.frame_background_color
        titlebar_text: root.active
            ? root.focused_titlebar_text_color
            : root.unfocused_titlebar_text_color
        titlebar_button_icon: root.titlebar_button_icon_color
        titlebar_button_hover: root.titlebar_button_hover_color
        titlebar_button_pressed: root.titlebar_button_pressed_color
        titlebar_close_hover: root.titlebar_close_hover_color
        titlebar_close_pressed: root.titlebar_close_pressed_color
        titlebar_activity_marker: root.active ? "#71b4ff" : "#5f7793"
        titlebar_content_border: "transparent"
        window_frame_border: root.frame_edge_color
    }

    VNM_NativeWindowFrame {
        window: root.Window.window
        frame_visible: false
        resize_enabled: root.resize_enabled
        resize_outward_margins.left: frame_shell.left_resize_outward_extent
        resize_outward_margins.top: frame_shell.top_resize_outward_extent
        resize_outward_margins.right: frame_shell.right_resize_outward_extent
        resize_outward_margins.bottom: frame_shell.bottom_resize_outward_extent
    }

    VNM_ChromeFrameShell {
        id: frame_shell
        objectName: "terminal_chrome_frame_shell"

        x: root.maximized_frame_overscan ? -root.shell_outer_edge_thickness : 0
        y: 0
        width: root.width
            + (root.maximized_frame_overscan ? 2 * root.shell_outer_edge_thickness : 0)
        height: root.height
            + (root.maximized_frame_overscan ? root.shell_outer_edge_thickness : 0)
        theme: terminal_chrome_theme
        frame_color: root.frame_background_color
        frame_outer_edge: root.shell_outer_edge_thickness
        frame_outer_edge_color: root.frame_edge_color
        frame_gap: root.shell_frame_gap
        frame_inner_edge: root.shell_inner_edge_thickness
        frame_inner_edge_color: root.frame_edge_color
        device_pixel_ratio: root.device_pixel_ratio
        titlebar_height: Math.min(root.titlebar_height, root.height)
        // Product invariant: the Varinomics mark aligns with the terminal
        // surface itself. The shared default instead clears the larger resize
        // hit band and moves the mark right of the surface. Keep this explicit
        // content-edge override when changing frame or titlebar geometry.
        titlebar_content_left_inset: frame_shell.content_interior_x
        title: root.title
        title_editing_enabled: root.title_editing_enabled
        title_reset_available: root.title_reset_available
        active: root.active
        maximized: root.maximized
        resize_enabled: root.resize_enabled
        activity_marker_text: root.activity_marker_text
        trailing_action_component: root.trailing_action_component
            ? root.trailing_action_component
            : (root.wheel_delivery_indicator_visible
                ? trailing_actions_component
                : null)
        custom_buttons: root.settings_button_visible
            ? [{
                object_name: "settings_button",
                component: settings_gear_component,
                tooltip: "Settings",
                action: function() { root.settings_requested() }
            }]
            : []

        onMove_requested: root.move_requested()
        onResize_requested: (edges) => root.resize_requested(edges)
        onMinimize_requested: root.minimize_requested()
        onMaximize_toggle_requested: root.maximize_toggle_requested()
        onClose_requested: root.close_requested()
        onTitle_edit_accepted: (title) => root.title_edit_accepted(title)
        onTitle_reset_requested: root.title_reset_requested()
    }

    // The settings gear is stroked here rather than taken from an icon font.
    // No single font family carries a gear on every platform, and the ones
    // that do disagree on how it looks, so a glyph would render as a different
    // drawing per system and as a missing-glyph box where none is installed.
    // Stroke weight and colour match the minimize/maximize/close marks the
    // chrome paints beside it.
    Component {
        id: settings_gear_component

        Canvas {
            anchors.fill: parent

            property color icon_color: terminal_chrome_theme.titlebar_button_icon

            // Proportions follow the Heroicons "cog-6-tooth" outline icon
            // (heroicons.com, MIT), measured off its path: a 24-unit box with
            // tip radius 9, root radius 6.75, hub radius 3, and a 1.5 stroke.
            // The geometry below is generated rather than traced, so it stays
            // resolution-independent and carries no bundled asset. The joins
            // depart from the reference deliberately; see the paint handler.
            readonly property int tooth_count: 6
            readonly property real stroke_width: 1.3
            // The reference draws its stroke at 1/6 of the tip radius. Solving
            // that for a 1.3 stroke gives a 7.8 radius, hence this box: the
            // stroke straddles the path, so the box also carries half a stroke
            // of inset to keep the teeth inside it.
            readonly property real icon_size: 17
            // The tip radius the reference would use for this box. Body, hub
            // and tip width are all measured against it, so extending the
            // teeth below moves nothing else.
            readonly property real reference_radius: icon_size / 2 - stroke_width / 2
            readonly property real root_radius: reference_radius * 0.75
            readonly property real hub_radius: reference_radius * 0.333
            // Teeth run longer than the reference's, which reads better at
            // this size. Only the tips move: they grow outward from the same
            // root circle, so the gear body and hub are untouched. The teeth
            // overshoot the nominal box by a fraction of a pixel, which the
            // button around them absorbs.
            readonly property real tooth_length_factor: 1.15
            readonly property real outer_radius:
                root_radius + (reference_radius - root_radius) * tooth_length_factor
            // Half-widths in radians of a tooth at its tip, on the outer
            // radius, and at its base, on the root radius. A gear tooth is a
            // trapezoid that narrows towards the tip, and arc length grows
            // with radius, so the tip needs the smaller share of the two: the
            // tooth only tapers outward while
            //     tip_half_angle < base_half_angle * (root_radius/outer_radius)
            // Equal angles would draw the flanks radially and flare the tooth
            // out into a petal instead. The reference tapers hard, to roughly
            // a third of the base width, which leaves the roots as narrow
            // valleys rather than flats.
            //
            // The tip half-angle is scaled back by the same factor the tips
            // moved out. An angle subtends more arc the further out it is, so
            // holding it fixed would have widened the tips as well as
            // lengthening them; this keeps the tip exactly as wide as the
            // reference's and lets the teeth only grow longer.
            readonly property real tip_half_angle: 0.144 * reference_radius / outer_radius
            readonly property real base_half_angle: 0.504
            // Half a tooth pitch, which for six teeth is 30 degrees. Teeth
            // generated straight from angle zero straddle the vertical and
            // leave a valley pointing up; the reference points a tooth up
            // instead, and this phase puts one back on the axis.
            readonly property real tooth_phase: Math.PI / tooth_count

            onIcon_colorChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = icon_color
                ctx.lineWidth = stroke_width
                // Mitred rather than the reference's round joins, which blunt
                // the teeth at this size. The limit is what keeps that safe:
                // a tooth tip turns gently enough to be mitred to a crisp
                // corner, while the far sharper turn at the root of a valley
                // exceeds the limit and falls back to a bevel instead of
                // growing the long spike an unbounded miter would produce.
                ctx.lineJoin = "miter"
                ctx.miterLimit = 2

                const cx = width / 2
                const cy = height / 2
                const step = 2 * Math.PI / tooth_count

                // One closed outline. Each arc() call first draws a straight
                // line from the previous arc's end to its own start, which is
                // exactly the flank between a tooth and the root beside it.
                ctx.beginPath()
                for (var i = 0; i < tooth_count; ++i) {
                    const tooth_centre = i * step + tooth_phase
                    ctx.arc(
                        cx, cy, outer_radius,
                        tooth_centre - tip_half_angle,
                        tooth_centre + tip_half_angle,
                        false)
                    ctx.arc(
                        cx, cy, root_radius,
                        tooth_centre + base_half_angle,
                        tooth_centre + step - base_half_angle,
                        false)
                }
                ctx.closePath()
                ctx.stroke()

                ctx.beginPath()
                ctx.arc(cx, cy, hub_radius, 0, 2 * Math.PI, false)
                ctx.stroke()
            }
        }
    }

    Component {
        id: trailing_actions_component

        Item {
            id: wheel_indicator
            width: 14
            height: Math.max(0, root.reduced_titlebar_height)

            Rectangle {
                anchors.centerIn: parent
                width: 7
                height: 7
                radius: width / 2
                color: "#ffdc2a"
                border.color: "#6c4a00"
                border.width: 1
                opacity: 0.92
            }
        }
    }

    // Row-timestamp hover label. It lives in the chrome because the chrome
    // root is the always-on-top layer over the terminal surface, so the
    // label can anchor to the hovered row's right end without a second
    // overlay mechanism. The panel touches the scrollbar's left edge when
    // the scrollbar is visible and the window content's right edge when it
    // is not, and fades in from the left, so it reads as part of the row
    // rather than as a floating card.
    Rectangle {
        id: row_timestamp_tooltip
        objectName: "row_timestamp_tooltip"

        readonly property real edge_margin: 4
        readonly property real fade_width: 48
        readonly property rect row_rect: root.row_timestamp_tooltip_row_rect
        // The row rect spans the surface, whose right edge is the scrollbar's
        // left edge; the window content's right edge lies past the scrollbar
        // strip.
        readonly property real anchor_right:
            root.row_timestamp_tooltip_scrollbar_visible
                ? row_rect.x + row_rect.width
                : root.content_interior_rect.x + root.content_interior_rect.width

        x: Math.max(edge_margin, anchor_right - width)
        y: Math.max(edge_margin, Math.min(
            row_rect.y + (row_rect.height - height) / 2,
            root.height - height - edge_margin))
        width: row_timestamp_tooltip_text.implicitWidth + fade_width + 8
        height: row_timestamp_tooltip_text.implicitHeight + 8
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0;  color: "transparent" }
            GradientStop { position: 0.35; color: "#161c26" }
            GradientStop { position: 1.0;  color: "#161c26" }
        }
        opacity: root.row_timestamp_tooltip_visible ? 1 : 0
        visible: opacity > 0
        // Pure feedback: the tooltip must never steal pointer events from
        // the terminal underneath it.
        enabled: false
        z: 1000

        Behavior on opacity { NumberAnimation { duration: 100 } }

        Text {
            id: row_timestamp_tooltip_text
            objectName: "row_timestamp_tooltip_text"
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: Qt.formatDateTime(
                root.row_timestamp_tooltip_timestamp, "yyyy-MM-dd hh:mm:ss")
            color: "#dfe5ee"
            font.pixelSize: 11
        }
    }
}
