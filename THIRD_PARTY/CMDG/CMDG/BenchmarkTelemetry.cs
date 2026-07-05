using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace CMDG;

public static class BenchmarkTelemetry
{
    private static readonly object WriteLock = new();
    private static readonly object WindowLock = new();
    private static readonly Stopwatch Elapsed = Stopwatch.StartNew();
    private static readonly List<WindowCounters> Windows = new();

    private static bool s_written;
    private static int s_sceneFrames;
    private static int s_drawFrames;
    private static long s_sceneCalcMsTotal;
    private static long s_sceneWaitMsTotal;
    private static long s_drawTicksTotal;
    private static long s_drawPreWriteBuildTicksTotal;
    private static long s_drawOpenStdoutTicksTotal;
    private static long s_drawUtf8EncodeTicksTotal;
    private static long s_drawStdoutWriteFlushTicksTotal;
    private static long s_drawWaitMsTotal;
    private static long s_drawOutputBytes;
    private static long s_changedRows;
    private static long s_changedCells;
    private static double s_lastSceneElapsedSeconds;
    private static double s_windowLastSceneElapsedSeconds;
    private static int s_frameLimitReached;

    private sealed class WindowCounters
    {
        public WindowCounters(int index, long intervalMs)
        {
            Index = index;
            StartElapsedMs = index * intervalMs;
        }

        public int Index { get; }
        public long StartElapsedMs { get; }
        public int SceneFrames;
        public int DrawFrames;
        public long SceneCalcMsTotal;
        public long SceneWaitMsTotal;
        public long DrawTicksTotal;
        public long DrawPreWriteBuildTicksTotal;
        public long DrawOpenStdoutTicksTotal;
        public long DrawUtf8EncodeTicksTotal;
        public long DrawStdoutWriteFlushTicksTotal;
        public long DrawWaitMsTotal;
        public long DrawOutputBytes;
        public long ChangedRows;
        public long ChangedCells;
        public bool HasSceneElapsedSeconds;
        public double SceneElapsedSecondsStart;
        public double SceneElapsedSecondsEnd;

        public WindowCounters Clone()
        {
            return new WindowCounters(Index, Config.BenchmarkWindowMs)
            {
                SceneFrames = SceneFrames,
                DrawFrames = DrawFrames,
                SceneCalcMsTotal = SceneCalcMsTotal,
                SceneWaitMsTotal = SceneWaitMsTotal,
                DrawTicksTotal = DrawTicksTotal,
                DrawPreWriteBuildTicksTotal = DrawPreWriteBuildTicksTotal,
                DrawOpenStdoutTicksTotal = DrawOpenStdoutTicksTotal,
                DrawUtf8EncodeTicksTotal = DrawUtf8EncodeTicksTotal,
                DrawStdoutWriteFlushTicksTotal = DrawStdoutWriteFlushTicksTotal,
                DrawWaitMsTotal = DrawWaitMsTotal,
                DrawOutputBytes = DrawOutputBytes,
                ChangedRows = ChangedRows,
                ChangedCells = ChangedCells,
                HasSceneElapsedSeconds = HasSceneElapsedSeconds,
                SceneElapsedSecondsStart = SceneElapsedSecondsStart,
                SceneElapsedSecondsEnd = SceneElapsedSecondsEnd,
            };
        }
    }

    public static bool Enabled => Config.BenchmarkMode;
    public static bool FrameLimitReached => Volatile.Read(ref s_frameLimitReached) != 0;
    public static string ExitReason => FrameLimitReached ? "frame_limit" : "normal_exit";

    public static void RecordSceneFrame(double sceneElapsedSeconds, int calcMs, int waitMs)
    {
        if (!Enabled) return;

        int frameCount = Interlocked.Increment(ref s_sceneFrames);
        Interlocked.Add(ref s_sceneCalcMsTotal, calcMs);
        Interlocked.Add(ref s_sceneWaitMsTotal, waitMs);
        Volatile.Write(ref s_lastSceneElapsedSeconds, sceneElapsedSeconds);
        RecordSceneWindow(sceneElapsedSeconds, calcMs, waitMs);

        if (Config.BenchmarkFrameLimit > 0 && frameCount >= Config.BenchmarkFrameLimit)
        {
            Volatile.Write(ref s_frameLimitReached, 1);
        }
    }

    public static void RecordDrawFrame(
        long drawTicks,
        long preWriteBuildTicks,
        long openStdoutTicks,
        long utf8EncodeTicks,
        long stdoutWriteFlushTicks,
        int waitMs,
        int outputBytes,
        int changedRows,
        int changedCells)
    {
        if (!Enabled) return;

        Interlocked.Increment(ref s_drawFrames);
        Interlocked.Add(ref s_drawTicksTotal, drawTicks);
        Interlocked.Add(ref s_drawPreWriteBuildTicksTotal, preWriteBuildTicks);
        Interlocked.Add(ref s_drawOpenStdoutTicksTotal, openStdoutTicks);
        Interlocked.Add(ref s_drawUtf8EncodeTicksTotal, utf8EncodeTicks);
        Interlocked.Add(ref s_drawStdoutWriteFlushTicksTotal, stdoutWriteFlushTicks);
        Interlocked.Add(ref s_drawWaitMsTotal, waitMs);
        Interlocked.Add(ref s_drawOutputBytes, outputBytes);
        Interlocked.Add(ref s_changedRows, changedRows);
        Interlocked.Add(ref s_changedCells, changedCells);
        RecordDrawWindow(
            drawTicks,
            preWriteBuildTicks,
            openStdoutTicks,
            utf8EncodeTicks,
            stdoutWriteFlushTicks,
            waitMs,
            outputBytes,
            changedRows,
            changedCells);
    }

    public static void WriteSummary(int exitCode, string exitReason)
    {
        if (!Enabled || string.IsNullOrWhiteSpace(Config.BenchmarkMetricsPath)) return;

        lock (WriteLock)
        {
            if (s_written) return;
            s_written = true;

            string? directory = Path.GetDirectoryName(Config.BenchmarkMetricsPath);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
            }

            File.WriteAllText(Config.BenchmarkMetricsPath, BuildJson(exitCode, exitReason));
        }
    }

    private static void RecordSceneWindow(double sceneElapsedSeconds, int calcMs, int waitMs)
    {
        long elapsedMs = Elapsed.ElapsedMilliseconds;
        int index = WindowIndexForElapsedMs(elapsedMs);

        lock (WindowLock)
        {
            WindowCounters window = EnsureWindow(index);
            if (!window.HasSceneElapsedSeconds)
            {
                window.SceneElapsedSecondsStart = s_windowLastSceneElapsedSeconds;
                window.HasSceneElapsedSeconds = true;
            }

            window.SceneFrames++;
            window.SceneCalcMsTotal += calcMs;
            window.SceneWaitMsTotal += waitMs;
            window.SceneElapsedSecondsEnd = sceneElapsedSeconds;
            s_windowLastSceneElapsedSeconds = sceneElapsedSeconds;
        }
    }

    private static void RecordDrawWindow(
        long drawTicks,
        long preWriteBuildTicks,
        long openStdoutTicks,
        long utf8EncodeTicks,
        long stdoutWriteFlushTicks,
        int waitMs,
        int outputBytes,
        int changedRows,
        int changedCells)
    {
        long elapsedMs = Elapsed.ElapsedMilliseconds;
        int index = WindowIndexForElapsedMs(elapsedMs);

        lock (WindowLock)
        {
            WindowCounters window = EnsureWindow(index);
            window.DrawFrames++;
            window.DrawTicksTotal += drawTicks;
            window.DrawPreWriteBuildTicksTotal += preWriteBuildTicks;
            window.DrawOpenStdoutTicksTotal += openStdoutTicks;
            window.DrawUtf8EncodeTicksTotal += utf8EncodeTicks;
            window.DrawStdoutWriteFlushTicksTotal += stdoutWriteFlushTicks;
            window.DrawWaitMsTotal += waitMs;
            window.DrawOutputBytes += outputBytes;
            window.ChangedRows += changedRows;
            window.ChangedCells += changedCells;
        }
    }

    private static int WindowIndexForElapsedMs(long elapsedMs)
    {
        return (int)(elapsedMs / Config.BenchmarkWindowMs);
    }

    private static WindowCounters EnsureWindow(int index)
    {
        while (Windows.Count <= index)
        {
            Windows.Add(new WindowCounters(Windows.Count, Config.BenchmarkWindowMs));
        }

        return Windows[index];
    }

    private static List<WindowCounters> SnapshotWindows(long elapsedMs)
    {
        int finalIndex = elapsedMs > 0
            ? WindowIndexForElapsedMs(elapsedMs - 1)
            : 0;

        lock (WindowLock)
        {
            EnsureWindow(finalIndex);

            var snapshot = new List<WindowCounters>(Windows.Count);
            foreach (WindowCounters window in Windows)
            {
                snapshot.Add(window.Clone());
            }
            return snapshot;
        }
    }

    private static double StopwatchTicksToMilliseconds(long ticks)
    {
        return ticks * 1000.0 / Stopwatch.Frequency;
    }

    private static string BuildJson(int exitCode, string exitReason)
    {
        int sceneFrames = Volatile.Read(ref s_sceneFrames);
        int drawFrames = Volatile.Read(ref s_drawFrames);
        long elapsedMs = Elapsed.ElapsedMilliseconds;
        double elapsedSeconds = Math.Max(Elapsed.Elapsed.TotalSeconds, 0.001);

        var json = new StringBuilder(1024);
        json.AppendLine("{");
        AppendJsonString(json, "scene", Config.SceneName, true);
        AppendJsonBoolean(json, "hide_cursor", Config.BenchmarkHideCursor, true);
        AppendJsonString(json, "exit_reason", exitReason, true);
        AppendJsonNumber(json, "exit_code", exitCode, true);
        AppendJsonNumber(json, "elapsed_ms", Elapsed.Elapsed.TotalMilliseconds, true);
        AppendJsonNumber(json, "scene_elapsed_seconds", Volatile.Read(ref s_lastSceneElapsedSeconds), true);
        AppendJsonNumber(json, "scene_frames", sceneFrames, true);
        AppendJsonNumber(json, "draw_frames", drawFrames, true);
        AppendJsonNumber(json, "scene_frames_per_second", sceneFrames / elapsedSeconds, true);
        AppendJsonNumber(json, "draw_frames_per_second", drawFrames / elapsedSeconds, true);
        AppendJsonNumber(json, "scene_calc_ms_total", Volatile.Read(ref s_sceneCalcMsTotal), true);
        AppendJsonNumber(json, "scene_wait_ms_total", Volatile.Read(ref s_sceneWaitMsTotal), true);
        AppendJsonNumber(
            json,
            "draw_ms_total",
            StopwatchTicksToMilliseconds(Volatile.Read(ref s_drawTicksTotal)),
            true);
        AppendJsonNumber(
            json,
            "draw_pre_write_build_ms_total",
            StopwatchTicksToMilliseconds(Volatile.Read(ref s_drawPreWriteBuildTicksTotal)),
            true);
        AppendJsonNumber(
            json,
            "draw_open_stdout_ms_total",
            StopwatchTicksToMilliseconds(Volatile.Read(ref s_drawOpenStdoutTicksTotal)),
            true);
        AppendJsonNumber(
            json,
            "draw_utf8_encode_ms_total",
            StopwatchTicksToMilliseconds(Volatile.Read(ref s_drawUtf8EncodeTicksTotal)),
            true);
        AppendJsonNumber(
            json,
            "draw_stdout_write_flush_ms_total",
            StopwatchTicksToMilliseconds(Volatile.Read(ref s_drawStdoutWriteFlushTicksTotal)),
            true);
        AppendJsonNumber(json, "draw_wait_ms_total", Volatile.Read(ref s_drawWaitMsTotal), true);
        AppendJsonNumber(json, "draw_output_bytes", Volatile.Read(ref s_drawOutputBytes), true);
        AppendJsonNumber(json, "changed_rows", Volatile.Read(ref s_changedRows), true);
        AppendJsonNumber(json, "changed_cells", Volatile.Read(ref s_changedCells), true);
        AppendWindowSummary(json, elapsedMs, false);
        json.AppendLine("}");
        return json.ToString();
    }

    private static void AppendWindowSummary(StringBuilder json, long elapsedMs, bool comma)
    {
        List<WindowCounters> windows = SnapshotWindows(elapsedMs);

        json.AppendLine("  \"benchmark_windows\": {");
        AppendJsonString(json, 4, "schema", "cmdg_benchmark_windows_v1", true);
        AppendJsonNumber(json, 4, "interval_ms", Config.BenchmarkWindowMs, true);
        json.AppendLine("    \"samples\": [");

        double carriedSceneElapsedSeconds = 0.0;
        for (int index = 0; index < windows.Count; index++)
        {
            WindowCounters window = windows[index];
            double sceneElapsedSecondsStart = carriedSceneElapsedSeconds;
            double sceneElapsedSecondsEnd = carriedSceneElapsedSeconds;
            if (window.HasSceneElapsedSeconds)
            {
                sceneElapsedSecondsStart = window.SceneElapsedSecondsStart;
                sceneElapsedSecondsEnd = window.SceneElapsedSecondsEnd;
                carriedSceneElapsedSeconds = sceneElapsedSecondsEnd;
            }

            long endElapsedMs = Math.Min(
                window.StartElapsedMs + Config.BenchmarkWindowMs,
                elapsedMs);
            double windowSeconds = Math.Max(
                (endElapsedMs - window.StartElapsedMs) / 1000.0,
                0.001);

            json.AppendLine("      {");
            AppendJsonNumber(json, 8, "index", window.Index, true);
            AppendJsonNumber(json, 8, "start_elapsed_ms", window.StartElapsedMs, true);
            AppendJsonNumber(json, 8, "end_elapsed_ms", endElapsedMs, true);
            AppendJsonNumber(json, 8, "scene_frames", window.SceneFrames, true);
            AppendJsonNumber(json, 8, "draw_frames", window.DrawFrames, true);
            AppendJsonNumber(
                json, 8, "scene_elapsed_seconds_start", sceneElapsedSecondsStart, true);
            AppendJsonNumber(
                json, 8, "scene_elapsed_seconds_end", sceneElapsedSecondsEnd, true);
            AppendJsonNumber(json, 8, "scene_calc_ms_total", window.SceneCalcMsTotal, true);
            AppendJsonNumber(json, 8, "scene_wait_ms_total", window.SceneWaitMsTotal, true);
            AppendJsonNumber(
                json,
                8,
                "draw_ms_total",
                StopwatchTicksToMilliseconds(window.DrawTicksTotal),
                true);
            AppendJsonNumber(
                json,
                8,
                "draw_pre_write_build_ms_total",
                StopwatchTicksToMilliseconds(window.DrawPreWriteBuildTicksTotal),
                true);
            AppendJsonNumber(
                json,
                8,
                "draw_open_stdout_ms_total",
                StopwatchTicksToMilliseconds(window.DrawOpenStdoutTicksTotal),
                true);
            AppendJsonNumber(
                json,
                8,
                "draw_utf8_encode_ms_total",
                StopwatchTicksToMilliseconds(window.DrawUtf8EncodeTicksTotal),
                true);
            AppendJsonNumber(
                json,
                8,
                "draw_stdout_write_flush_ms_total",
                StopwatchTicksToMilliseconds(window.DrawStdoutWriteFlushTicksTotal),
                true);
            AppendJsonNumber(json, 8, "draw_wait_ms_total", window.DrawWaitMsTotal, true);
            AppendJsonNumber(json, 8, "draw_output_bytes", window.DrawOutputBytes, true);
            AppendJsonNumber(json, 8, "changed_rows", window.ChangedRows, true);
            AppendJsonNumber(json, 8, "changed_cells", window.ChangedCells, true);
            AppendJsonNumber(
                json, 8, "scene_frames_per_second", window.SceneFrames / windowSeconds, true);
            AppendJsonNumber(
                json, 8, "draw_frames_per_second", window.DrawFrames / windowSeconds, false);
            json.Append("      }");
            json.AppendLine(index + 1 == windows.Count ? "" : ",");
        }

        json.AppendLine("    ]");
        json.Append("  }");
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendIndent(StringBuilder json, int spaces)
    {
        json.Append(' ', spaces);
    }

    private static void AppendJsonString(StringBuilder json, string name, string value, bool comma)
    {
        AppendJsonString(json, 2, name, value, comma);
    }

    private static void AppendJsonString(
        StringBuilder json,
        int indent,
        string name,
        string value,
        bool comma)
    {
        AppendIndent(json, indent);
        json.Append('"').Append(name).Append("\": \"").Append(Escape(value)).Append('"');
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendJsonNumber(StringBuilder json, string name, double value, bool comma)
    {
        AppendJsonNumber(json, 2, name, value, comma);
    }

    private static void AppendJsonNumber(
        StringBuilder json,
        int indent,
        string name,
        double value,
        bool comma)
    {
        AppendIndent(json, indent);
        json.Append('"').Append(name).Append("\": ");
        json.Append(value.ToString("0.###", CultureInfo.InvariantCulture));
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendJsonNumber(StringBuilder json, string name, long value, bool comma)
    {
        AppendJsonNumber(json, 2, name, value, comma);
    }

    private static void AppendJsonNumber(
        StringBuilder json,
        int indent,
        string name,
        long value,
        bool comma)
    {
        AppendIndent(json, indent);
        json.Append('"').Append(name).Append("\": ").Append(value);
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendJsonBoolean(StringBuilder json, string name, bool value, bool comma)
    {
        AppendIndent(json, 2);
        json.Append('"').Append(name).Append("\": ");
        json.Append(value ? "true" : "false");
        json.AppendLine(comma ? "," : "");
    }

    private static string Escape(string value)
    {
        return value
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\r", "\\r", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);
    }
}
