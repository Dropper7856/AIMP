using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class LauncherResetTests
{
    internal static Task GeneratedDataScopeAsync()
    {
        var temporary = NewTemporaryDirectory();
        try
        {
            var data = Path.Combine(temporary, "AIMP");
            var game = Path.Combine(temporary, "game");
            var settings = Write(data, "launcher-settings.json");
            var cache = Write(data, "WorldManifestCache/map.aimpwm");
            var receipt = Write(data, "Preparation/receipt.json");
            var manifest = Write(game, "DATA/ENV/PRODUCTION/MISSION/WORLD/WORLD_MANIFEST_map.aimpwm");
            var json = Write(game, "DATA/ENV/PRODUCTION/DLC/CHALLENGE/WORLD/WORLD_MANIFEST_map.json");
            var commands = Write(game, "DATA/ENV/PRODUCTION/MISSION/WORLD/COMMANDS.PAK");
            var backup = Write(data, "backups/ui/UI.PAK.original");
            var journal = Write(game, "AIMP/PreparationBackups/Ripley/transaction.json");
            var save = Write(game, "saves/player.sav");
            AssertEx.Equal(2, LauncherResetFiles.DeleteGeneratedData(data, settings, game));
            foreach (var removed in new[] { settings, cache, receipt, manifest, json }) AssertEx.False(File.Exists(removed));
            foreach (var kept in new[] { commands, backup, journal, save }) AssertEx.Equal("original", File.ReadAllText(kept));
        }
        finally { Cleanup(temporary); }
        return Task.CompletedTask;
    }

    internal static Task RefusesOutsideSettingsPathAsync()
    {
        var temporary = NewTemporaryDirectory();
        try
        {
            var data = Path.Combine(temporary, "AIMP");
            var cache = Write(data, "WorldManifestCache/map.aimpwm");
            var outside = Write(temporary, "another-app/settings.json");
            AssertEx.Throws<IOException>(() => LauncherResetFiles.DeleteGeneratedData(data, outside, string.Empty));
            AssertEx.True(File.Exists(cache), "Invalid reset scope must be rejected before deleting any data.");
            AssertEx.Equal("original", File.ReadAllText(outside));
        }
        finally { Cleanup(temporary); }
        return Task.CompletedTask;
    }

    private static string NewTemporaryDirectory()
    {
        var directory = Path.Combine(Path.GetTempPath(), "AIMP.ResetTests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        return directory;
    }

    private static string Write(string root, string relative)
    {
        var path = Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "original");
        return path;
    }

    private static void Cleanup(string directory)
    {
        var full = Path.GetFullPath(directory);
        if (!string.Equals(Path.GetDirectoryName(full)?.TrimEnd(Path.DirectorySeparatorChar),
                Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase)
            || !Path.GetFileName(full).StartsWith("AIMP.ResetTests-", StringComparison.Ordinal))
            throw new IOException("Refusing cleanup outside the reset test directory.");
        Directory.Delete(full, recursive: true);
    }
}
