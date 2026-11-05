using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class DeveloperModeTests
{
    internal static Task StartupArgumentsAsync()
    {
        var hostArguments = new[] { "--urls", "http://127.0.0.1:5127" };
        AssertEx.SequenceEqual(hostArguments, LauncherStartupOptions.Parse(hostArguments, out var available));
        AssertEx.False(available);
        AssertEx.SequenceEqual(hostArguments, LauncherStartupOptions.Parse(
            new[] { "-dev", "--urls", "http://127.0.0.1:5127", "-DEV" }, out available));
        AssertEx.True(available);
        AssertEx.SequenceEqual(new[] { "-developer", "--dev" }, LauncherStartupOptions.Parse(
            new[] { "-developer", "--dev" }, out available));
        AssertEx.False(available);
        return Task.CompletedTask;
    }

    internal static Task SessionGateAsync()
    {
        using var temporary = new TemporaryDirectory();
        var path = temporary.Combine("settings.json");
        var developer = new LauncherSettingsStore(path, developerModeAvailable: true);
        var normal = new LauncherSettingsStore(path);
        AssertEx.False(normal.DeveloperModeAvailable);
        AssertEx.True(developer.DeveloperModeAvailable);
        // -dev exposes the toggle; it does not turn the saved mode on by itself.
        AssertEx.False(developer.Load().DeveloperMode);
        developer.Save(new LauncherSettings("C:\\Game", "Ripley"));
        developer.SetDeveloperMode(true);
        AssertEx.True(developer.Load().DeveloperMode);
        AssertEx.False(normal.Load().DeveloperMode, "A saved preference cannot enable a normal session.");
        AssertEx.Throws<InvalidOperationException>(() => normal.SetDeveloperMode(true));
        AssertEx.True(developer.Load().DeveloperMode, "A rejected toggle must not alter saved settings.");
        developer.SetDeveloperMode(false);
        AssertEx.False(developer.Load().DeveloperMode);
        normal.Save(normal.Load() with { DeveloperMode = true });
        AssertEx.False(developer.Load().DeveloperMode, "Normal-session saves cannot enable the mode.");
        AssertEx.Equal("Ripley", normal.Load().PlayerName);
        return Task.CompletedTask;
    }
}
