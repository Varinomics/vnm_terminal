#include "app_profile_text.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "vnm_terminal/diagnostics/profile_text.h"
#include "vnm_terminal/internal/profile_text_writers.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QTextStream>

namespace vnm_terminal::terminal_app {

namespace term  = vnm_terminal::internal;
namespace diag  = vnm_terminal::diagnostics;

bool prepare_profile_text_file(
    const QString& path,
    QString*       out_error)
{
    if (path.trimmed().isEmpty()) {
        *out_error = QStringLiteral("--profile-text requires a non-empty path");
        return false;
    }

    const QFileInfo file_info(path);
    const QDir parent_dir = file_info.absoluteDir();
    if (!parent_dir.exists()) {
        *out_error = QStringLiteral("--profile-text parent directory does not exist: %1")
            .arg(parent_dir.absolutePath());
        return false;
    }
    if (file_info.exists() && file_info.isDir()) {
        *out_error = QStringLiteral("--profile-text points to a directory: %1")
            .arg(file_info.absoluteFilePath());
        return false;
    }

    return true;
}

bool write_profile_text(
    const QString&                     path,
    VNM_TerminalSurface&               surface,
    const term::Hierarchical_profiler& gui_profiler,
    QString*                           out_error)
{
    const term::Profile_timeline_snapshot gui_timeline = gui_profiler.timeline_snapshot();

    QString text;
    QTextStream stream(&text);
    stream << "vnm_terminal example terminal profile\n";
    stream << "format=2\n";
    stream << "time_unit=ns\n\n";
    diag::append_surface_geometry_profile_text(surface, stream);
    stream << '\n';
    diag::append_dirty_row_stats_text(surface, stream);
    stream << '\n';
    diag::append_dirty_row_timeline_text(surface, stream);
    stream << '\n';
    diag::append_model_profile_stats_text(surface, stream);
    stream << '\n';
    diag::append_retained_history_profile_text(surface, stream);
    stream << '\n';
    diag::append_session_profile_stats_text(surface, stream);
    stream << '\n';
    diag::append_qsg_atlas_profile_text(surface, stream);
    stream << '\n';
    diag::append_slow_text_layout_diagnostics_text(surface, stream);
    stream << "\ngui_thread\n";
    diag::append_profile_node_text(stream, gui_profiler.root_snapshot(), 1);
    stream << '\n';
    diag::append_profile_timeline_text(stream, QStringLiteral("gui_thread"), gui_timeline);
    stream << '\n';
    diag::append_render_thread_profile_text(surface, stream);
    stream.flush();

    const QString absolute_path = QFileInfo(path).absoluteFilePath();
    QFile file(absolute_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        *out_error = QStringLiteral("could not write profile text %1: %2")
            .arg(absolute_path, file.errorString());
        return false;
    }

    const QByteArray profile_bytes = text.toUtf8();
    if (file.write(profile_bytes) != profile_bytes.size()) {
        *out_error = QStringLiteral("could not write profile text %1: %2")
            .arg(absolute_path, file.errorString());
        return false;
    }

    return true;
}

} // namespace vnm_terminal::terminal_app

#endif
