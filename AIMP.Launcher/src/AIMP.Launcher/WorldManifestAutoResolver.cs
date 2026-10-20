namespace AIMP.Launcher;

// Only DLC levels are supported until cooperative campaign support is enabled.
internal static class WorldManifestAutoResolver
{
    internal static string ResolveLevelSource(string gameDirectory, string level)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(level);
        var requested = level.Trim();
        if (requested.Length == 0 || requested.Contains("..", StringComparison.Ordinal)
            || requested.Contains('\\') || requested.Contains('/') || requested.Contains(':'))
            throw new InvalidOperationException("The lobby level name is not a safe map identifier.");
        var matches = WorldManifestLevelDiscovery.Discover(gameDirectory)
            .Where(source => source.CanCompile && string.Equals(
                Path.GetFileName(source.LevelDirectory), requested, StringComparison.OrdinalIgnoreCase))
            .Take(2).ToArray();
        if (matches.Length != 1)
            throw new InvalidOperationException(matches.Length == 0
                ? $"DLC level '{level}' was not found. Campaign levels are not supported yet."
                : $"DLC level '{level}' is ambiguous in this installation.");
        return matches[0].LevelDirectory;
    }
}
