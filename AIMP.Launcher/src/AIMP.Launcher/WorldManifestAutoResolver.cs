namespace AIMP.Launcher;

// Resolves the current lobby level to its retail WORLD source without a broad
// recursive filesystem scan. WorldSync uses this only once per level start;
// COMMANDS parsing/manifest compilation remains launcher-side and cached.
internal static class WorldManifestAutoResolver
{
    internal static string ResolveLevelSource(string gameDirectory, string level)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(level);

        var installation = GameInstallation.Resolve(gameDirectory);
        var production = Path.Combine(
            installation.GameDirectory, "DATA", "ENV", "PRODUCTION");
        if (!Directory.Exists(production))
            throw new DirectoryNotFoundException(
                $"Alien: Isolation production environment directory was not found: '{production}'.");

        var requested = level.Trim().Trim('\\', '/');
        if (requested.Length == 0
            || requested.Contains("..", StringComparison.Ordinal)
            || requested.Contains('\\')
            || requested.Contains('/')
            || requested.Contains(':'))
            throw new InvalidOperationException("The lobby level name is not a safe map identifier.");

        // Fast normal path used by retail campaign/challenge maps.
        var direct = Path.Combine(production, requested);
        if (IsLevelSource(direct)) return direct;

        // Case-insensitive top-level lookup for Windows installations whose
        // lobby spelling differs from the on-disk directory casing.
        foreach (var directory in Directory.EnumerateDirectories(
                     production, "*", SearchOption.TopDirectoryOnly))
        {
            if (string.Equals(Path.GetFileName(directory), requested,
                    StringComparison.OrdinalIgnoreCase)
                && IsLevelSource(directory))
                return directory;
        }

        // Some DLC/custom layouts group maps one directory below PRODUCTION.
        // Keep this bounded to exactly one grouping level; never recurse over
        // the full game tree during gameplay.
        foreach (var group in Directory.EnumerateDirectories(
                     production, "*", SearchOption.TopDirectoryOnly))
        {
            foreach (var directory in Directory.EnumerateDirectories(
                         group, "*", SearchOption.TopDirectoryOnly))
            {
                if (string.Equals(Path.GetFileName(directory), requested,
                        StringComparison.OrdinalIgnoreCase)
                    && IsLevelSource(directory))
                    return directory;
            }
        }

        throw new DirectoryNotFoundException(
            $"No WORLD/COMMANDS.PAK or COMMANDS.BIN source was found for lobby level '{level}'.");
    }

    private static bool IsLevelSource(string candidate)
    {
        if (!Directory.Exists(candidate)) return false;
        var world = Path.Combine(candidate, "WORLD");
        return File.Exists(Path.Combine(world, "COMMANDS.PAK"))
            || File.Exists(Path.Combine(world, "COMMANDS.BIN"))
            || File.Exists(Path.Combine(candidate, "COMMANDS.PAK"))
            || File.Exists(Path.Combine(candidate, "COMMANDS.BIN"));
    }
}
