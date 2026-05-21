#include "vnm_terminal/vnm_terminal_surface.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThread>

#include <iostream>

namespace {

int fail(const char* message)
{
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int count_dark_background_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() >= 6 && color.red() <= 14 &&
                color.green() >= 9 && color.green() <= 16 &&
                color.blue() >= 13 && color.blue() <= 20)
            {
                ++count;
            }
        }
    }

    return count;
}

int count_light_background_pixels(const QImage& image, const QRect& area)
{
    int         count   = 0;
    const QRect bounded = area.intersected(image.rect());

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() >= 240 && color.red() <= 252 &&
                color.green() >= 240 && color.green() <= 252 &&
                color.blue() >= 235 && color.blue() <= 248)
            {
                ++count;
            }
        }
    }

    return count;
}

bool pump_until_rendered(
    QGuiApplication&   app,
    QQuickWindow&      window,
    QImage&            out,
    bool               light_theme)
{
    for (int i = 0; i < 20; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
        out = window.grabWindow();

        if (out.isNull()) {
            continue;
        }

        const QRect probe(250, 120, 40, 24);
        const int background_pixels = light_theme
            ? count_light_background_pixels(out, probe)
            : count_dark_background_pixels(out, probe);
        if (background_pixels > 600) {
            return true;
        }
    }

    return false;
}

}

int main(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.setColor(QColor(180, 16, 16));
    window.resize(320, 160);

    VNM_TerminalSurface surface;
    surface.setParentItem(window.contentItem());
    surface.setSize(QSizeF(320.0, 160.0));
    surface.set_font_family(QStringLiteral("monospace"));
    surface.set_font_size(18.0);

    if (!surface.flags().testFlag(QQuickItem::ItemHasContents)) {
        return fail("terminal surface does not declare scene graph content");
    }

    if (!surface.clip()) {
        return fail("terminal surface must clip scene graph content to its item bounds");
    }

    if (!surface.childItems().empty()) {
        return fail("terminal surface must not allocate child QQuickItems for smoke text");
    }

    window.show();

    QImage rendered;
    if (!pump_until_rendered(app, window, rendered, false)) {
        return fail("terminal surface did not render the dark empty terminal background");
    }

    if (count_dark_background_pixels(rendered, QRect(250, 120, 40, 24)) < 600) {
        return fail("terminal surface did not preserve dark background outside the smoke text");
    }

    if (!surface.childItems().empty()) {
        return fail("terminal surface created child QQuickItems during smoke rendering");
    }

    if (rendered.size().width() < 160 || rendered.size().height() < 80) {
        return fail("grabbed render target is unexpectedly small");
    }

    surface.set_color_theme(QStringLiteral("light"));
    QImage light_rendered;
    if (!pump_until_rendered(app, window, light_rendered, true)) {
        return fail("terminal surface did not render the light empty terminal background");
    }

    if (count_dark_background_pixels(light_rendered, QRect(250, 120, 40, 24)) > 20) {
        return fail("light-theme render did not repaint the background");
    }

    return 0;
}
