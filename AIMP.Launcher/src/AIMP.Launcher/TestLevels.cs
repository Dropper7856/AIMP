namespace AIMP.Launcher;

/// <summary>
/// Developer test levels (flat platforms built with OpenCAGE) shipped beside the
/// launcher API in <c>TestLevels\&lt;NAME&gt;</c>. They are installed under
/// <c>DATA\ENV\PRODUCTION\&lt;NAME&gt;</c> so the engine path (<c>Production\&lt;NAME&gt;</c>)
/// and every native PRODUCTION/ assumption hold, and are only offered in
/// Developer mode. The lobby treats them as Survival; the frontend loads them
/// directly with load_level, as it does an individual campaign level.
/// </summary>
internal static class TestLevels
{
    internal static readonly IReadOnlyList<string> Names = ["WORLDSYNC_TEST_LEVEL", "AISYNC_TEST_LEVEL"];

    // Written by the deploy step into the shipped folder, and copied into the
    // installed one last: equal stamps mean the installation is current.
    internal const string StampFile = "AIMP_TESTLEVEL.SHA256";

    internal static bool IsTestLevel(string level) =>
        Names.Contains(level.Trim(), StringComparer.OrdinalIgnoreCase);

    internal static string SourceRoot => Path.Combine(AppContext.BaseDirectory, "TestLevels");

    /// <summary>Installs or refreshes every shipped test level. Returns the names installed now.</summary>
    internal static IReadOnlyList<string> Install(string gameDirectory, CancellationToken token)
        => Install(gameDirectory, SourceRoot, token);

    internal static IReadOnlyList<string> Install(string gameDirectory, string sourceRoot, CancellationToken token)
    {
        var production = WorldManifestLevelDiscovery.GetProductionDirectory(gameDirectory);
        var installed = new List<string>();
        foreach (var name in Names)
        {
            token.ThrowIfCancellationRequested();
            var source = Path.Combine(sourceRoot, name);
            var sourceStamp = ReadStamp(source);
            if (sourceStamp is null) continue;
            var target = Path.Combine(production, name);
            if (string.Equals(ReadStamp(target), sourceStamp, StringComparison.Ordinal)) continue;
            CopyLevel(source, target, token);
            installed.Add(name);
        }
        // Never edit MAIN.PKG: the retail Steam AI.exe checks the integrity of its
        // package files at boot, before AIMP is injected, and exits (code 0)
        // before the menu when MAIN.PKG differs (build 552 regression). Undo the
        // edit that build made, from the backup it kept.
        RestoreOriginalPackage(gameDirectory);
        return installed;
    }

    internal static bool RestoreOriginalPackage(string gameDirectory)
    {
        var path = Path.Combine(gameDirectory, "DATA", "PACKAGES", "MAIN.PKG");
        var backup = path + ".aimp-backup";
        if (!File.Exists(backup)) return false;
        File.Copy(backup, path, overwrite: true);
        File.Delete(backup);
        return true;
    }

    private static string? ReadStamp(string directory)
    {
        var path = Path.Combine(directory, StampFile);
        try
        {
            return File.Exists(path) ? File.ReadAllText(path).Trim() : null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static void CopyLevel(string source, string target, CancellationToken token)
    {
        // A stale copy (older build, or patched by the character step) is replaced
        // whole: the character step patches the fresh copy again afterwards.
        if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            token.ThrowIfCancellationRequested();
            var relative = Path.GetRelativePath(source, file);
            if (relative.Equals(StampFile, StringComparison.OrdinalIgnoreCase)) continue;
            var destination = Path.Combine(target, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(file, destination, overwrite: true);
        }
        // Last, so an interrupted copy is redone at the next preparation.
        File.Copy(Path.Combine(source, StampFile), Path.Combine(target, StampFile), overwrite: true);
    }
}
