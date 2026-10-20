namespace AIMP.Launcher;

internal sealed record WorldManifestLevelSource(
    string DisplayName,
    string RelativePath,
    string LevelDirectory,
    bool CanCompile,
    string ValidationMessage);

internal static class WorldManifestLevelDiscovery
{
    private const int MaxDiscoveryDepth = 16;

    internal static IReadOnlyList<WorldManifestLevelSource> Discover(string gameDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameDirectory);

        var production = GetProductionDirectory(gameDirectory);
        var dlc = GetDlcDirectory(gameDirectory);
        if (!Directory.Exists(dlc))
        {
            throw new DirectoryNotFoundException(
                $"Alien: Isolation DLC directory was not found: '{dlc}'.");
        }

        var levels = new List<WorldManifestLevelSource>();
        var pending = new Stack<(string Directory, int Depth)>();
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // Campaign support is disabled for this release. Never scan its levels.
        pending.Push((dlc, 0));

        while (pending.Count != 0)
        {
            var (parent, depth) = pending.Pop();
            if (depth >= MaxDiscoveryDepth) continue;

            foreach (var directory in EnumerateDirectoriesSafe(parent))
            {
                var fullPath = Path.GetFullPath(directory);
                if (!visited.Add(fullPath)) continue;

                if (IsLevelDirectory(fullPath))
                {
                    levels.Add(CreateLevelSource(production, fullPath));
                    // RENDERABLE/WORLD marks a complete level. Do not descend into
                    // the level's internal asset tree looking for nested maps.
                    continue;
                }

                if (!IsReparsePoint(fullPath)) pending.Push((fullPath, depth + 1));
            }
        }

        return levels
            .OrderBy(level => level.RelativePath, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    internal static string GetProductionDirectory(string gameDirectory)
    {
        var installation = GameInstallation.Resolve(gameDirectory);
        return Path.Combine(installation.GameDirectory, "DATA", "ENV", "PRODUCTION");
    }

    internal static string GetDlcDirectory(string gameDirectory) =>
        Path.Combine(GetProductionDirectory(gameDirectory), "DLC");

    private static WorldManifestLevelSource CreateLevelSource(
        string production,
        string levelDirectory)
    {
        var relative = Path.GetRelativePath(production, levelDirectory)
            .Trim(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var world = Path.Combine(levelDirectory, "WORLD");
        var hasCommands = File.Exists(Path.Combine(world, "COMMANDS.PAK"))
            || File.Exists(Path.Combine(world, "COMMANDS.BIN"))
            || File.Exists(Path.Combine(levelDirectory, "COMMANDS.PAK"))
            || File.Exists(Path.Combine(levelDirectory, "COMMANDS.BIN"));

        return new WorldManifestLevelSource(
            relative,
            relative,
            levelDirectory,
            hasCommands,
            hasCommands
                ? "Ready"
                : "WORLD/COMMANDS.PAK and WORLD/COMMANDS.BIN were not found.");
    }

    private static bool IsLevelDirectory(string directory) =>
        Directory.Exists(Path.Combine(directory, "RENDERABLE"))
        && Directory.Exists(Path.Combine(directory, "WORLD"));

    private static IEnumerable<string> EnumerateDirectoriesSafe(string directory)
    {
        try
        {
            return Directory.EnumerateDirectories(directory, "*", SearchOption.TopDirectoryOnly)
                .ToArray();
        }
        catch (Exception exception) when (
            exception is UnauthorizedAccessException or IOException)
        {
            return Array.Empty<string>();
        }
    }

    private static bool IsReparsePoint(string directory)
    {
        try
        {
            return (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0;
        }
        catch (Exception exception) when (
            exception is UnauthorizedAccessException or IOException)
        {
            return true;
        }
    }
}
