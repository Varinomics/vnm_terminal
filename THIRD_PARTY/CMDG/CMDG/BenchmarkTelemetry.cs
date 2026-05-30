using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace CMDG;

public static class BenchmarkTelemetry
{
    private static readonly object WriteLock = new();
    private static readonly Stopwatch Elapsed = Stopwatch.StartNew();

    private static bool s_written;
    private static int s_sceneFrames;
    private static int s_drawFrames;
    private static long s_sceneCalcMsTotal;
    private static long s_sceneWaitMsTotal;
    private static long s_drawMsTotal;
    private static long s_drawWaitMsTotal;
    private static long s_drawOutputBytes;
    private static long s_changedRows;
    private static long s_changedCells;
    private static double s_lastSceneElapsedSeconds;
    private static int s_frameLimitReached;

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

        if (Config.BenchmarkFrameLimit > 0 && frameCount >= Config.BenchmarkFrameLimit)
        {
            Volatile.Write(ref s_frameLimitReached, 1);
        }
    }

    public static void RecordDrawFrame(
        int drawMs,
        int waitMs,
        int outputBytes,
        int changedRows,
        int changedCells)
    {
        if (!Enabled) return;

        Interlocked.Increment(ref s_drawFrames);
        Interlocked.Add(ref s_drawMsTotal, drawMs);
        Interlocked.Add(ref s_drawWaitMsTotal, waitMs);
        Interlocked.Add(ref s_drawOutputBytes, outputBytes);
        Interlocked.Add(ref s_changedRows, changedRows);
        Interlocked.Add(ref s_changedCells, changedCells);
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

    private static string BuildJson(int exitCode, string exitReason)
    {
        int sceneFrames = Volatile.Read(ref s_sceneFrames);
        int drawFrames = Volatile.Read(ref s_drawFrames);
        double elapsedSeconds = Math.Max(Elapsed.Elapsed.TotalSeconds, 0.001);

        var json = new StringBuilder(1024);
        json.AppendLine("{");
        AppendJsonString(json, "scene", Config.SceneName, true);
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
        AppendJsonNumber(json, "draw_ms_total", Volatile.Read(ref s_drawMsTotal), true);
        AppendJsonNumber(json, "draw_wait_ms_total", Volatile.Read(ref s_drawWaitMsTotal), true);
        AppendJsonNumber(json, "draw_output_bytes", Volatile.Read(ref s_drawOutputBytes), true);
        AppendJsonNumber(json, "changed_rows", Volatile.Read(ref s_changedRows), true);
        AppendJsonNumber(json, "changed_cells", Volatile.Read(ref s_changedCells), false);
        json.AppendLine("}");
        return json.ToString();
    }

    private static void AppendJsonString(StringBuilder json, string name, string value, bool comma)
    {
        json.Append("  \"").Append(name).Append("\": \"").Append(Escape(value)).Append('"');
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendJsonNumber(StringBuilder json, string name, double value, bool comma)
    {
        json.Append("  \"").Append(name).Append("\": ");
        json.Append(value.ToString("0.###", CultureInfo.InvariantCulture));
        json.AppendLine(comma ? "," : "");
    }

    private static void AppendJsonNumber(StringBuilder json, string name, long value, bool comma)
    {
        json.Append("  \"").Append(name).Append("\": ").Append(value);
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
