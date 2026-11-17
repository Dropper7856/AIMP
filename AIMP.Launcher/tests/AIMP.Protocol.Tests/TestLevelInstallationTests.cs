using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class TestLevelInstallationTests
{
    // The retail AI.exe checks MAIN.PKG's integrity at boot and exits before
    // the menu when it differs: installing test levels must never edit it, and
    // must put back the original that build 552 replaced (kept as .aimp-backup).
    internal static Task RegistrationAsync()
    {
        using var temporary = new TemporaryDirectory();
        var game = temporary.Combine("game");
        var source = temporary.Combine("shipped");
        var package = Path.Combine(game, "DATA", "PACKAGES", "MAIN.PKG");
        Directory.CreateDirectory(Path.GetDirectoryName(package)!);
        Directory.CreateDirectory(Path.Combine(game, "DATA", "ENV", "PRODUCTION"));
        var original = "<package id=\"Main\"><game_data><levels /></game_data></package>"u8.ToArray();
        File.WriteAllBytes(package + ".aimp-backup", original);
        File.WriteAllText(package, "<package id=\"Main\"><game_data><levels><level id=\"Production/WORLDSYNC_TEST_LEVEL\" /></levels></game_data></package>");
        foreach (var name in TestLevels.Names)
        {
            var folder = Path.Combine(source, name);
            Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder, TestLevels.StampFile), "fixture-stamp");
            File.WriteAllText(Path.Combine(folder, "fixture.dat"), "level content");
        }

        AssertEx.Equal(2, TestLevels.Install(game, source, CancellationToken.None).Count);
        AssertEx.SequenceEqual(original, File.ReadAllBytes(package));
        AssertEx.True(!File.Exists(package + ".aimp-backup"), "The backup is consumed once restored.");
        AssertEx.Equal(0, TestLevels.Install(game, source, CancellationToken.None).Count);
        AssertEx.SequenceEqual(original, File.ReadAllBytes(package));
        return Task.CompletedTask;
    }
}
