namespace CMDG
{
    // Constant configuration values
    public static class Config
    {
        public static string SceneName = EnvString("CMDG_SCENE", "AssemblyWinter2025"); // Select the scene to play by entering its name. Must be a class that contains a Run() method.
        /*
        Available scenes:

        SceneTemplate:      Copy this to create a new scene
        Example2D:          Simple example of a 2D scene (randomly moving pixels)
        Plasma:             Simple 2D plasma demo effect
        Example3D:          Simple example of a 3D scene
        CarRotateTest:      View a car 3D model and rotate the camera with arrow keys / WASD
        AssemblyWinter2025: "Nelostie" demo for Assembly Winter 2025
        Particle_vortex:    Turbo Knight music video for Assembly Winter 2026. Includes disk rendering option
        */



        public const int MaxFrameRate = 60;
        public const int ScreenWidth = 310;         
        public const int ScreenHeight = 75;
        public const bool ShowTime = false;                   // Display draw/calc/wait milliseconds below screen
        public const bool DoubleWidth = false;                // Use two character blocks to display one "pixel". (Looks more square, but requires more space and processing time.)
        public static Color32 BackgroundColor = new Color32(0, 0, 0);   // Wipe the framebuffer with this color before drawing
        public static char DefaultCharacter = 'X';            // Character to use as a 'pixel'. This is the default character if none is specified. Can be changed on the fly.
        public static bool MultipleCharacters = true;        // Allows using other characters besides the default. Makes drawing slower, so use only if you intend to use multiple characters in one frame.
        public static bool FullBlockCharacter = false;        // Use a full block character █ and background color instead of above character 
        public static bool BenchmarkMode = EnvFlag("CMDG_BENCHMARK", false);
        public static int BenchmarkFrameLimit = EnvInt("CMDG_BENCHMARK_FRAME_LIMIT", 0);
        public static string BenchmarkMetricsPath = EnvString("CMDG_BENCHMARK_METRICS", "");
        public static bool DisableAudio = EnvFlag("CMDG_DISABLE_AUDIO", BenchmarkMode);
        public static bool AdjustScreen = EnvFlag("CMDG_ADJUST_SCREEN", false); // Instructions to adjust screen at startup
        public static bool SplashScreen = EnvFlag("CMDG_SPLASH_SCREEN", !BenchmarkMode); // CMDG splash screen after screen adjustment and before demo.
        public static bool EndScreen = false;                 // End screen after quitting
        public static bool ReadConsoleFirst = false;          // Save existing console contents into Util.ReadCharacters (x, y, char) when starting up the program.
        public static bool DiskRenderer = false;              // Save each frame to "frames/NNNNNN.bin" instead of drawing to console. Use bin2png to make video afterwards.

        private static string EnvString(string name, string fallback)
        {
            string? value = Environment.GetEnvironmentVariable(name);
            return string.IsNullOrWhiteSpace(value) ? fallback : value;
        }

        private static bool EnvFlag(string name, bool fallback)
        {
            string? value = Environment.GetEnvironmentVariable(name);
            if (string.IsNullOrWhiteSpace(value)) return fallback;

            value = value.Trim();
            if (value.Equals("1", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("true", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("yes", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("on", StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            if (value.Equals("0", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("false", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("no", StringComparison.OrdinalIgnoreCase) ||
                value.Equals("off", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            return fallback;
        }

        private static int EnvInt(string name, int fallback)
        {
            string? value = Environment.GetEnvironmentVariable(name);
            return int.TryParse(value, out int parsed) ? parsed : fallback;
        }
    }
}
