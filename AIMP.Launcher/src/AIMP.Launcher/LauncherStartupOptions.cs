namespace AIMP.Launcher;

internal static class LauncherStartupOptions
{
    internal static string[] Parse(string[] arguments, out bool developerModeAvailable)
    {
        developerModeAvailable = arguments.Any(IsDeveloperSwitch);
        // -dev is a launcher switch, not an ASP.NET configuration key.
        return arguments.Where(argument => !IsDeveloperSwitch(argument)).ToArray();
    }

    private static bool IsDeveloperSwitch(string argument) =>
        string.Equals(argument, "-dev", StringComparison.OrdinalIgnoreCase);
}
