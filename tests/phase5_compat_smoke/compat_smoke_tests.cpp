#include "vnm_terminal/internal/terminal_canvas_fixture_contract.h"
#include "vnm_terminal/internal/vnm_terminal_surface_render_bridge.h"
#include "vnm_terminal/vnm_terminal_surface.h"
#include "helpers/test_check.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace term = vnm_terminal::internal;

namespace {

using vnm_terminal::test_helpers::check;

bool argument_equals(const char* argument, const char* expected)
{
    return std::string_view(argument) == expected;
}

void pump_events(QGuiApplication& app, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

template <typename Predicate>
bool pump_until(QGuiApplication& app, Predicate predicate, int timeout_ms = 10000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        app.processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) {
            return true;
        }

        QThread::msleep(10);
    }

    app.processEvents(QEventLoop::AllEvents, 50);
    return predicate();
}

struct surface_grid_size_t
{
    int rows       = 0;
    int columns    = 0;
};

surface_grid_size_t current_surface_grid(const VNM_TerminalSurface& surface)
{
    return {surface.rows(), surface.columns()};
}

bool surface_grid_size_is_positive(surface_grid_size_t grid)
{
    return grid.rows > 0 && grid.columns > 0;
}

bool same_surface_grid_size(surface_grid_size_t lhs, surface_grid_size_t rhs)
{
    return lhs.rows == rhs.rows && lhs.columns == rhs.columns;
}

QString snapshot_row_text(
    const term::Terminal_render_snapshot&  snapshot,
    int                                    row)
{
    QString text;
    for (int column = 0; column < snapshot.grid_size.columns; ++column) {
        QString cell_text = QStringLiteral(" ");
        for (const term::Terminal_render_cell& cell : snapshot.cells) {
            if (cell.position.row == row && cell.position.column == column) {
                cell_text = cell.text;
                break;
            }
        }
        text += cell_text;
    }

    while (!text.isEmpty() && text.back() == QChar(u' ')) {
        text.chop(1);
    }
    return text;
}

std::vector<QString> snapshot_rows(const term::Terminal_render_snapshot& snapshot)
{
    std::vector<QString> rows;
    rows.reserve(static_cast<std::size_t>(snapshot.grid_size.rows));
    for (int row = 0; row < snapshot.grid_size.rows; ++row) {
        rows.push_back(snapshot_row_text(snapshot, row));
    }
    return rows;
}

bool snapshot_contains_text(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (const QString& row : snapshot_rows(snapshot)) {
        if (row.contains(text)) {
            return true;
        }
    }

    return false;
}

bool snapshot_contains_row(
    const term::Terminal_render_snapshot&  snapshot,
    const QString&                         text)
{
    for (const QString& row : snapshot_rows(snapshot)) {
        if (row == text) {
            return true;
        }
    }

    return false;
}

std::shared_ptr<const term::Terminal_render_snapshot> current_snapshot(
    VNM_TerminalSurface& surface)
{
    return term::VNM_TerminalSurface_render_bridge::render_snapshot(surface);
}

bool wait_for_snapshot_text(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const QString&         text)
{
    return pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        return snapshot != nullptr && snapshot_contains_text(*snapshot, text);
    });
}

bool wait_for_snapshot_row(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const QString&         text)
{
    return pump_until(app, [&] {
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        return snapshot != nullptr && snapshot_contains_row(*snapshot, text);
    });
}

bool wait_for_surface_resize(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    surface_grid_size_t    initial_grid,
    surface_grid_size_t&   resized_grid)
{
    return pump_until(app, [&] {
        const surface_grid_size_t grid = current_surface_grid(surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (!surface.backend_geometry_in_sync()        ||
            !surface_grid_size_is_positive(grid)       ||
            same_surface_grid_size(grid, initial_grid) ||
            snapshot                    == nullptr     ||
            snapshot->grid_size.rows    != grid.rows   ||
            snapshot->grid_size.columns != grid.columns)
        {
            return false;
        }

        resized_grid = grid;
        return true;
    });
}

bool wait_for_surface_initial_sync(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    surface_grid_size_t&   grid)
{
    return pump_until(app, [&] {
        const surface_grid_size_t current_grid = current_surface_grid(surface);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (!surface.backend_geometry_in_sync()              ||
            !surface_grid_size_is_positive(current_grid)     ||
            snapshot                    == nullptr           ||
            snapshot->grid_size.rows    != current_grid.rows ||
            snapshot->grid_size.columns != current_grid.columns)
        {
            return false;
        }

        grid = current_grid;
        return true;
    });
}

QString stream_line_for_count(int count)
{
    return QStringLiteral("stream-row-%1").arg(count, 3, 10, QChar(u'0'));
}

QString size_line_for_grid(
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t&
                           contract,
    surface_grid_size_t    grid)
{
    return QStringLiteral("%1%2x%3")
        .arg(QString::fromLatin1(
            contract.size_prefix.data(),
            static_cast<qsizetype>(contract.size_prefix.size())))
        .arg(grid.rows)
        .arg(grid.columns);
}

bool wait_for_size_report(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t&
                           contract,
    surface_grid_size_t    grid,
    int                    timeout_ms = 10000)
{
    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    const QString expected_size_row = prompt + size_line_for_grid(contract, grid);
    const QString size_command = QString::fromLatin1(
        contract.size_command.data(),
        static_cast<qsizetype>(contract.size_command.size()));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto next_query = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= next_query) {
            if (!surface.paste_text(size_command + QLatin1Char('\n'))) {
                return false;
            }
            next_query = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(250);
        }

        app.processEvents(QEventLoop::AllEvents, 50);
        const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
            current_snapshot(surface);
        if (snapshot != nullptr && snapshot_contains_row(*snapshot, expected_size_row)) {
            return true;
        }

        QThread::msleep(10);
    }

    app.processEvents(QEventLoop::AllEvents, 50);
    const std::shared_ptr<const term::Terminal_render_snapshot> snapshot =
        current_snapshot(surface);
    return snapshot != nullptr && snapshot_contains_row(*snapshot, expected_size_row);
}

bool is_live_surface_process_state(VNM_TerminalSurface::Process_state state)
{
    return
        state == VNM_TerminalSurface::Process_state::STARTING ||
        state == VNM_TerminalSurface::Process_state::RUNNING;
}

struct Surface_fixture
{
    QQuickWindow           window;
    VNM_TerminalSurface    surface;

    Surface_fixture()
    {
        window.resize(720, 360);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(620.0, 260.0));
        surface.set_font_family(QStringLiteral("monospace"));
        surface.set_font_size(12.0);
        surface.set_bracketed_paste_policy(
            VNM_TerminalSurface::Bracketed_paste_policy::DISABLED);
        window.show();
    }
};

bool wait_for_process_exit(
    QGuiApplication&       app,
    bool&                  process_exited,
    int                    timeout_ms = 10000)
{
    return pump_until(app, [&] {
        return process_exited;
    }, timeout_ms);
}

bool cleanup_surface_process(
    QGuiApplication&       app,
    VNM_TerminalSurface&   surface,
    bool&                  process_exited,
    std::string_view       label)
{
    pump_events(app);
    if (process_exited ||
        !is_live_surface_process_state(surface.process_state()))
    {
        return true;
    }

    const bool terminate_requested = surface.terminate_process();
    const bool exited              = wait_for_process_exit(app, process_exited, 5000);
    pump_events(app);

    if (!terminate_requested) {
        std::cerr << label << ": failed to request process termination during cleanup\n";
    }

    if (!exited) {
        std::cerr << label << ": process did not exit after cleanup termination\n";
    }

    return terminate_requested && exited;
}

void print_backend_errors(std::string_view label, const QStringList& backend_errors)
{
    for (const QString& backend_error : backend_errors) {
        std::cerr << label << " backend error: "
            << backend_error.toLocal8Bit().constData() << '\n';
    }
}

bool test_shell_like_surface_native_smoke(QGuiApplication& app, const QString& fixture_path)
{
    bool ok = true;
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();
    ok &= check(contract.stream_count > 0 &&
        contract.stream_max_count > 0 &&
        contract.stream_count <= contract.stream_max_count,
        "shell-like stream count is positive and bounded");
    if (!ok) {
        return false;
    }

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const bool started = fixture.surface.start_process(
        {
            fixture_path,
            QStringLiteral("--shell-like-smoke"),
        },
        QFileInfo(fixture_path).absolutePath());
    ok &= check(started, "shell-like native surface smoke starts fixture");
    if (!started) {
        return false;
    }

    auto require = [&](bool condition, const std::string& message) -> bool {
        if (check(condition, message)) {
            return true;
        }

        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like native surface smoke");
        return false;
    };

    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    QString visible_prompt = prompt;
    while (!visible_prompt.isEmpty() && visible_prompt.back() == QChar(u' ')) {
        visible_prompt.chop(1);
    }
    if (!require(
            visible_prompt.size() >= 2, "shell-like visible prompt contract is non-empty after trimming"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_row(app, fixture.surface, visible_prompt), "shell-like prompt reaches surface snapshot"))
    {
        return false;
    }

    const QString echo_text = QString::fromLatin1(
        contract.echo_text.data(),
        static_cast<qsizetype>(contract.echo_text.size()));
    if (!require(
            fixture.surface.paste_text(
                QStringLiteral("%1 %2\n")
                    .arg(QString::fromLatin1(
                        contract.echo_command.data(),
                        static_cast<qsizetype>(contract.echo_command.size())))
                    .arg(echo_text)),
            "shell-like echo command writes through public paste_text"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_text(app, fixture.surface, echo_text), "shell-like echo output reaches surface snapshot"))
    {
        return false;
    }

    surface_grid_size_t initial_grid;
    if (!require(
            wait_for_surface_initial_sync(app, fixture.surface, initial_grid),
            "shell-like surface publishes synchronized initial native grid"))
    {
        return false;
    }

    if (!require(
            wait_for_size_report(app, fixture.surface, contract, initial_grid),
            "shell-like size output reports exact initial surface rows and columns"))
    {
        return false;
    }

    fixture.window.resize(900, 500);
    fixture.surface.setSize(QSizeF(820.0, 420.0));

    surface_grid_size_t resized_grid;
    if (!require(
            wait_for_surface_resize(app, fixture.surface, initial_grid, resized_grid),
            "shell-like surface resize changes synchronized native grid"))
    {
        return false;
    }

    if (!require(
            wait_for_size_report(app, fixture.surface, contract, resized_grid),
            "shell-like size output reports exact resized surface rows and columns"))
    {
        return false;
    }

    if (!require(
            contract.stream_count > resized_grid.rows, "shell-like stream count exceeds resized visible rows"))
    {
        return false;
    }

    if (!require(
            fixture.surface.paste_text( QStringLiteral("stream %1\n").arg(contract.stream_count)),
            "shell-like stream command writes through public paste_text"))
    {
        return false;
    }

    const QString final_stream_line = stream_line_for_count(contract.stream_count);
    if (!require(
            wait_for_snapshot_row(app, fixture.surface, final_stream_line),
            "shell-like derived stream final line reaches surface snapshot"))
    {
        return false;
    }

    const std::shared_ptr<const term::Terminal_render_snapshot> stream_snapshot =
        current_snapshot(fixture.surface);
    if (!require(
            stream_snapshot != nullptr, "shell-like stream publishes a render snapshot"))
    {
        return false;
    }

    const int minimum_scrollback_rows = std::max(1, contract.stream_count - resized_grid.rows);
    ok &= check(stream_snapshot->grid_size.rows == resized_grid.rows &&
        stream_snapshot->grid_size.columns == resized_grid.columns,
        "shell-like stream snapshot keeps resized surface grid");
    ok &= check(stream_snapshot->viewport.scrollback_rows >= minimum_scrollback_rows,
        "shell-like stream produces scrollback for rows beyond the resized viewport");
    ok &= check(stream_snapshot->viewport.scrollback_rows <=
        contract.stream_count + resized_grid.rows + 8,
        "shell-like stream scrollback stays within expected smoke-test bounds");
    ok &= check(snapshot_contains_row(*stream_snapshot, final_stream_line),
        "shell-like stream final row remains exact in current snapshot");
    if (!ok) {
        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like native surface smoke");
        return false;
    }

    if (!require(
            fixture.surface.paste_text(QStringLiteral("exit\n")),
            "shell-like exit command writes through public paste_text"))
    {
        return false;
    }

    if (!require(
            wait_for_process_exit(app, process_exited), "shell-like fixture exits before timeout"))
    {
        return false;
    }
    pump_events(app);

    print_backend_errors("shell-like native surface", backend_errors);

    ok &= check(exit_reason == VNM_TerminalSurface::Exit_reason::EXITED &&
        process_exit_code == 0,
        "shell-like fixture exits cleanly");
    ok &= check(backend_error_count == 0,
        "shell-like native surface smoke has no backend errors");
    ok &= check(fixture.surface.process_state() ==
        VNM_TerminalSurface::Process_state::EXITED,
        "shell-like surface publishes exited process state");

    return ok;
}

bool test_shell_like_surface_interrupt_smoke(QGuiApplication& app, const QString& fixture_path)
{
    bool ok = true;
    const term::terminal_canvas_fixture_shell_like_smoke_contract_t contract =
        term::terminal_canvas_fixture_shell_like_smoke_contract();

    Surface_fixture fixture;
    pump_events(app);

    int backend_error_count = 0;
    QStringList backend_errors;
    bool process_exited = false;
    VNM_TerminalSurface::Exit_reason exit_reason =
        VNM_TerminalSurface::Exit_reason::FAILED_TO_START;
    int process_exit_code = -1;

    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::backend_error,
        &fixture.surface,
        [&](VNM_TerminalSurface::Backend_error_code code, const QString& message) {
            ++backend_error_count;
            backend_errors.push_back(
                QStringLiteral("%1: %2")
                    .arg(static_cast<int>(code))
                    .arg(message));
        });
    QObject::connect(
        &fixture.surface,
        &VNM_TerminalSurface::process_exited,
        &fixture.surface,
        [&](VNM_TerminalSurface::Exit_reason reason, int exit_code) {
            process_exited    = true;
            exit_reason       = reason;
            process_exit_code = exit_code;
        });

    const bool started = fixture.surface.start_process(
        {
            fixture_path,
            QStringLiteral("--shell-like-smoke"),
        },
        QFileInfo(fixture_path).absolutePath());
    ok &= check(started, "shell-like interrupt surface smoke starts fixture");
    if (!started) {
        return false;
    }

    auto require = [&](bool condition, const std::string& message) -> bool {
        if (check(condition, message)) {
            return true;
        }

        (void)cleanup_surface_process(
            app,
            fixture.surface,
            process_exited,
            "shell-like interrupt surface smoke");
        return false;
    };

    const QString prompt = QString::fromLatin1(
        contract.prompt.data(),
        static_cast<qsizetype>(contract.prompt.size()));
    QString visible_prompt = prompt;
    while (!visible_prompt.isEmpty() && visible_prompt.back() == QChar(u' ')) {
        visible_prompt.chop(1);
    }
    if (!require(
            visible_prompt.size() >= 2, "shell-like interrupt visible prompt contract is non-empty after trimming"))
    {
        return false;
    }

    if (!require(
            wait_for_snapshot_row(app, fixture.surface, visible_prompt),
            "shell-like interrupt prompt reaches surface snapshot"))
    {
        return false;
    }

    if (!require(
            fixture.surface.paste_text(
                QStringLiteral("%1\n")
                    .arg(QString::fromLatin1(
                        contract.wait_command.data(),
                        static_cast<qsizetype>(contract.wait_command.size())))),
            "shell-like wait command writes through public paste_text"))
    {
        return false;
    }

    const QString wait_output = QString::fromLatin1(
        contract.wait_output.data(),
        static_cast<qsizetype>(contract.wait_output.size()));
    if (!require(
            wait_for_snapshot_row(app, fixture.surface, prompt + wait_output),
            "shell-like wait command reaches blocking fixture path"))
    {
        return false;
    }

    if (!require(
            fixture.surface.interrupt_process(), "shell-like wait path accepts public interrupt"))
    {
        return false;
    }

    if (!require(
            wait_for_process_exit(app, process_exited), "shell-like interrupted fixture exits before timeout"))
    {
        return false;
    }
    pump_events(app);

    print_backend_errors("shell-like interrupt surface", backend_errors);

    ok &= check(exit_reason == VNM_TerminalSurface::Exit_reason::INTERRUPTED &&
        process_exit_code == 130,
        "shell-like wait path exits through interrupt");
    ok &= check(backend_error_count == 0,
        "shell-like interrupt smoke has no backend errors");
    ok &= check(fixture.surface.process_state() ==
        VNM_TerminalSurface::Process_state::EXITED,
        "shell-like interrupted surface publishes exited process state");

    return ok;
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    if (argc != 2 || argument_equals(argv[1], "--help")) {
        std::cerr << "usage: compat_smoke_tests <fixture-executable>\n";
        return 2;
    }

    const QString fixture_path = QString::fromLocal8Bit(argv[1]);
    bool ok = true;
    ok &= test_shell_like_surface_native_smoke(app, fixture_path);
    ok &= test_shell_like_surface_interrupt_smoke(app, fixture_path);
    return ok ? 0 : 1;
}
