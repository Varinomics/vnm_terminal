#include "app_profile_text.h"

#if VNM_TERMINAL_PROFILING_ENABLED

#include "vnm_terminal/diagnostics/profile_text.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QTextStream>

#include <chrono>
#include <cstdint>

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

qint64 profile_nanoseconds(std::chrono::nanoseconds duration)
{
    return static_cast<qint64>(duration.count());
}

qint64 profile_mean_nanoseconds(
    std::chrono::nanoseconds   total_time,
    std::uint64_t              call_count)
{
    return call_count == 0U
        ? 0
        : static_cast<qint64>(
            total_time.count() / static_cast<std::int64_t>(call_count));
}

void append_profile_node_text(
    QTextStream&                           stream,
    const term::Profile_node_snapshot&     node,
    int                                    depth)
{
    const QString indent(depth * 2, QLatin1Char(' '));
    stream
        << indent
        << QString::fromStdString(node.name)
        << " calls="    << static_cast<qulonglong>(node.call_count)
        << " total_ns=" << profile_nanoseconds(node.total_time)
        << " mean_ns="  << profile_mean_nanoseconds(node.total_time, node.call_count)
        << " self_ns="  << profile_nanoseconds(node.self_time)
        << " child_ns=" << profile_nanoseconds(node.child_time)
        << " min_ns="   << profile_nanoseconds(node.min_time)
        << " max_ns="   << profile_nanoseconds(node.max_time)
        << '\n';

    for (const term::Profile_node_snapshot& child : node.children) {
        append_profile_node_text(stream, child, depth + 1);
    }
}

void append_profile_timeline_text(
    QTextStream&                           stream,
    const QString&                         label,
    const term::Profile_timeline_snapshot& timeline)
{
    stream
        << label
        << "_timeline bucket_width_ms="
        << static_cast<qulonglong>(timeline.bucket_width.count())
        << " buckets=" << static_cast<qulonglong>(timeline.buckets.size())
        << '\n';

    for (const term::Profile_timeline_bucket_snapshot& bucket : timeline.buckets) {
        if (bucket.scopes.empty()) {
            continue;
        }

        stream
            << "  bucket start_ms="
            << static_cast<qulonglong>(bucket.start_time.count())
            << " end_ms=" << static_cast<qulonglong>(bucket.end_time.count())
            << " scopes=" << static_cast<qulonglong>(bucket.scopes.size())
            << '\n';
        for (const term::Profile_timeline_scope_snapshot& scope : bucket.scopes) {
            stream
                << "    " << QString::fromStdString(scope.name)
                << " calls="    << static_cast<qulonglong>(scope.call_count)
                << " total_ns=" << profile_nanoseconds(scope.total_time)
                << " mean_ns="
                << profile_mean_nanoseconds(scope.total_time, scope.call_count)
                << " min_ns="   << profile_nanoseconds(scope.min_time)
                << " max_ns="   << profile_nanoseconds(scope.max_time)
                << '\n';
        }
    }
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
    append_profile_node_text(stream, gui_profiler.root_snapshot(), 1);
    stream << '\n';
    append_profile_timeline_text(stream, QStringLiteral("gui_thread"), gui_timeline);
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
