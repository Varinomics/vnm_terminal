#include "helpers/test_check.h"
#include <QByteArray>
#include <QColor>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPoint>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTextLayout>
#include <QTextOption>
#include <QThread>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>

namespace {

#ifndef VNM_TERMINAL_TEST_MONOSPACE_FONT_PATH
#define VNM_TERMINAL_TEST_MONOSPACE_FONT_PATH ""
#endif

constexpr qreal k_cell_width        = 16.0;
constexpr qreal k_cell_height       = 28.0;
constexpr int   k_red_column        = 2;
constexpr int   k_green_column      = 8;
constexpr int   k_window_width      = 220;
constexpr int   k_window_height     = 96;
constexpr int   k_logger_pixel_size = 13;

QString& loaded_font_family()
{
    static QString family;
    return family;
}

using vnm_terminal::test_helpers::check;

bool has_argument(int argc, char** argv, const char* expected)
{
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == expected) {
            return true;
        }
    }

    return false;
}

bool is_red_text_pixel(QColor color)
{
    return color.red() > 150 && color.green() < 100 && color.blue() < 120;
}

bool is_green_text_pixel(QColor color)
{
    return color.green() > 140 && color.red() < 120 && color.blue() < 140;
}

bool is_cell_background_pixel(QColor color)
{
    return
        color.red()   >= 26 &&
        color.red()   <= 46 &&
        color.green() >= 26 &&
        color.green() <= 46 &&
        color.blue()  >= 30 &&
        color.blue()  <= 54;
}

int count_matching_pixels(
    const QImage&                      image,
    QRect                              area,
    const std::function<bool(QColor)>& matches)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (matches(image.pixelColor(x, y))) {
                ++count;
            }
        }
    }

    return count;
}

QRect matching_pixel_bounds(const QImage& image, const std::function<bool(QColor)>& matches)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!matches(image.pixelColor(x, y))) {
                continue;
            }

            const QPoint point(x, y);
            bounds = bounds.isNull() ? QRect(point, QSize(1, 1)) : bounds.united(QRect(point, QSize(1, 1)));
        }
    }

    return bounds;
}

QRect logical_cell_rect(int column, int column_count = 1)
{
    return
        QRect(
            static_cast<int>(std::round(static_cast<qreal>(column) * k_cell_width)),
            0,
            static_cast<int>(std::round(static_cast<qreal>(column_count) * k_cell_width)),
            static_cast<int>(std::round(k_cell_height)));
}

qreal image_scale(const QImage& image)
{
    return image.isNull()
        ? 1.0
        : static_cast<qreal>(image.width()) / static_cast<qreal>(k_window_width);
}

QRect image_cell_rect(const QImage& image, int column, int column_count = 1)
{
    const qreal scale = image_scale(image);
    return
        QRect(
            static_cast<int>(std::round(static_cast<qreal>(column) * k_cell_width * scale)),
            0,
            static_cast<int>(std::round(static_cast<qreal>(column_count) * k_cell_width * scale)),
            static_cast<int>(std::round(k_cell_height * scale)));
}

int minimum_text_pixels(const QImage& image, int column_count)
{
    const QRect area = image_cell_rect(image, 0, column_count);
    return std::max(16, static_cast<int>(
        std::round(static_cast<qreal>(area.width() * area.height()) * 0.02)));
}

QFont text_node_font()
{
    QFont font(loaded_font_family());
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPixelSize(k_logger_pixel_size);
    return font;
}

void prepare_text_layout(QTextLayout& layout)
{
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(k_cell_width * 12.0);
    line.setPosition(QPointF(0.0, 0.0));
    layout.endLayout();
}

void append_rect(QSGNode& parent, const QRectF& rect, const QColor& color)
{
    auto* node = new QSGSimpleRectNode(rect, color);
    node->setFlag(QSGNode::OwnedByParent, true);
    parent.appendChildNode(node);
}

bool append_text_run(
    QSGNode&       parent,
    QQuickWindow&  window,
    const QString& text,
    QColor         color,
    int            column)
{
    QTextLayout layout(text, text_node_font());
    prepare_text_layout(layout);

    QSGTextNode* node = window.createTextNode();
    if (node == nullptr) {
        std::cerr << "FAIL: QQuickWindow::createTextNode returned null\n";
        return false;
    }

    node->setColor(color);
    // Keep the check on Qt's own glyph path. Native rendering can be evaluated
    // separately if a target platform needs it.
    node->setRenderType(QSGTextNode::QtRendering);
    node->setViewport(QRectF(0.0, 0.0, k_window_width, k_window_height));
    node->addTextLayout(QPointF(static_cast<qreal>(column) * k_cell_width, 2.0), &layout);
    parent.appendChildNode(node);
    return true;
}

class Qsg_text_node_test_item final : public QQuickItem
{
public:
    Qsg_text_node_test_item()
    {
        setFlag(ItemHasContents, true);
        setSize(QSizeF(k_window_width, k_window_height));
    }

    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override
    {
        delete old_node;

        QQuickWindow* render_window = window();
        if (render_window == nullptr) {
            return nullptr;
        }

        auto* root = new QSGNode();
        m_text_node_creation_failed.store(false);
        append_rect(*root, QRectF(0.0, 0.0, width(), height()),          QColor(8, 10, 14));
        append_rect(*root, QRectF(logical_cell_rect(k_red_column, 2)),   QColor(34, 34, 40));
        append_rect(*root, QRectF(logical_cell_rect(k_green_column, 2)), QColor(34, 34, 40));

        const bool red_ok = append_text_run(
            *root,
            *render_window,
            QStringLiteral("HH"),
            QColor(245, 35, 45),
            k_red_column);
        const bool green_ok = append_text_run(
            *root,
            *render_window,
            QStringLiteral("HH"),
            QColor(35, 230, 80),
            k_green_column);
        if (!red_ok || !green_ok) {
            m_text_node_creation_failed.store(true);
        }
        return root;
    }

    bool text_node_creation_failed() const
    {
        return m_text_node_creation_failed.load();
    }

private:
    std::atomic_bool m_text_node_creation_failed = false;
};

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
}

QImage render_window_until(
    QGuiApplication&                           app,
    QQuickWindow&                              window,
    const std::function<bool(const QImage&)>&  predicate)
{
    QImage image;
    for (int i = 0; i < 30; ++i) {
        pump_events(app, 1);
        image = window.grabWindow();
        if (!image.isNull() && predicate(image)) {
            return image;
        }
    }

    return image;
}

bool test_public_qsg_text_node_route(QGuiApplication& app)
{
    bool ok = true;

    QQuickWindow window;
    window.setColor(QColor(80, 0, 80));
    window.resize(k_window_width, k_window_height);

    Qsg_text_node_test_item item;
    item.setParentItem(window.contentItem());

    window.show();
    item.update();

    const QImage image = render_window_until(app, window, [](const QImage& candidate) {
        const QRect red_rect   = image_cell_rect(candidate, k_red_column,   2);
        const QRect green_rect = image_cell_rect(candidate, k_green_column, 2);
        return
            count_matching_pixels(candidate, red_rect, is_red_text_pixel)     > minimum_text_pixels(candidate, 2) &&
            count_matching_pixels(candidate, green_rect, is_green_text_pixel) > minimum_text_pixels(candidate, 2);
    });

    const QRect red_bounds   = matching_pixel_bounds(image, is_red_text_pixel);
    const QRect green_bounds = matching_pixel_bounds(image, is_green_text_pixel);
    const int expected_delta = static_cast<int>(
        std::round((k_green_column - k_red_column) * k_cell_width * image_scale(image)));
    const int   actual_delta = green_bounds.left() - red_bounds.left();
    const QRect red_cell     = image_cell_rect(image, k_red_column);
    const QRect green_cell   = image_cell_rect(image, k_green_column);
    const int   red_pixels   =
        count_matching_pixels(image, image_cell_rect(image, k_red_column, 2), is_red_text_pixel);
    const int green_pixels    = count_matching_pixels(
        image,
        image_cell_rect(image, k_green_column, 2),
        is_green_text_pixel);
    const int delta_tolerance = std::max(2, static_cast<int>(std::ceil(2.0 * image_scale(image))));

    ok &= check(!image.isNull(), "QSGTextNode route produced a window grab");
    ok &= check(!item.text_node_creation_failed(), "QQuickWindow::createTextNode succeeded");
    ok &= check(red_bounds.isValid(), "red QSGTextNode text run is visible");
    ok &= check(green_bounds.isValid(), "green QSGTextNode text run is visible");
    ok &= check(red_pixels > minimum_text_pixels(image, 2),
        "red text run has a meaningful colored pixel footprint");
    ok &= check(green_pixels > minimum_text_pixels(image, 2),
        "green text run has a meaningful colored pixel footprint");
    ok &= check(red_bounds.left() >= red_cell.left() &&
        red_bounds.left() < image_cell_rect(image, k_red_column + 1).left(),
        "red text run starts inside its requested terminal cell");
    ok &= check(green_bounds.left() >= green_cell.left() &&
        green_bounds.left() < image_cell_rect(image, k_green_column + 1).left(),
        "green text run starts inside its requested terminal cell");
    ok &= check(std::abs(actual_delta - expected_delta) <= delta_tolerance,
        "colored text runs preserve the requested cell-origin x delta");
    ok &= check(count_matching_pixels(image, image_cell_rect(image, k_green_column, 2), is_red_text_pixel) <
        std::max(3, red_pixels / 8),
        "red text run does not materially bleed into the green run cells");
    ok &= check(count_matching_pixels(image, image_cell_rect(image, k_red_column, 2), is_green_text_pixel) <
        std::max(3, green_pixels / 8),
        "green text run does not materially bleed into the red run cells");
    ok &= check(count_matching_pixels(image, image_cell_rect(image, k_red_column, 2), is_cell_background_pixel) > 80,
        "red run background is separate scene graph rectangle content");
    ok &= check(count_matching_pixels(image, image_cell_rect(image, k_green_column, 2), is_cell_background_pixel) > 80,
        "green run background is separate scene graph rectangle content");

    std::cout << "INFO: QSGTextNode public route rendered colored QTextLayout runs at cell x origins "
        << logical_cell_rect(k_red_column).left() << " and "
        << logical_cell_rect(k_green_column).left()
        << " with observed text x delta " << actual_delta
        << " at grab scale " << image_scale(image) << ".\n";
    return ok;
}

bool load_vnm_framework_monospace_font()
{
    const QString font_path = QString::fromUtf8(VNM_TERMINAL_TEST_MONOSPACE_FONT_PATH);
    const int     font_id   = QFontDatabase::addApplicationFont(font_path);
    if (font_id < 0) {
        std::cerr << "FAIL: could not load VNM monospace font fixture: "
            << font_path.toLocal8Bit().constData() << '\n';
        return false;
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty()) {
        std::cerr << "FAIL: VNM monospace font fixture has no application family\n";
        return false;
    }

    loaded_font_family() = families.front();
    std::cout << "INFO: loaded VNM framework monospace font family: "
        << loaded_font_family().toLocal8Bit().constData()
        << " at pixel size " << k_logger_pixel_size << ".\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // The registered CTest path uses the repository's software/offscreen posture.
    // Running the executable without this flag probes the default Qt graphics backend.
    if (has_argument(argc, argv, "--software-renderer")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QGuiApplication app(argc, argv);
    if (!load_vnm_framework_monospace_font()) {
        return 1;
    }

    return test_public_qsg_text_node_route(app) ? 0 : 1;
}
