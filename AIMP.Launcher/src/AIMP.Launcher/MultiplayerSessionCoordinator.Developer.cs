namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private async Task SetDeveloperModeAsync(ScaleformBridgeMessage message)
    {
        var value = message.Get("enabled");
        if (value is not ("0" or "1")) throw new InvalidOperationException("Invalid developer mode.");
        var current = _settingsStore.Load();
        _settingsStore.Save(current with { DeveloperMode = value == "1" });
        await SendVoiceSettingsAsync().ConfigureAwait(false);
        await SendConfigAsync().ConfigureAwait(false);
    }

    private static void ValidateDeveloperLobby(LauncherSettings settings, ScaleformBridgeMessage message)
    {
        var level = ResolveLevel(message);
        var mode = message.Get("mode", "survival");
        if (!settings.DeveloperMode && !mode.Equals("survival", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Enable Developer mode to create an experimental campaign lobby.");
        if (settings.DeveloperMode)
            WorldManifestAutoResolver.ResolveLevelSource(settings.GameDirectory, level, includeCampaign: true);
    }

    private static string LevelGameMode(string level) =>
        level.StartsWith("Challenge", StringComparison.OrdinalIgnoreCase)
        || level.StartsWith("BSP", StringComparison.OrdinalIgnoreCase)
        || level.StartsWith("Salvage", StringComparison.OrdinalIgnoreCase) ? "Survival" : "Campaign";

    private static Dictionary<string, string> AddDeveloperLevels(
        LauncherSettings settings, Dictionary<string, string> values)
    {
        values["developerMode"] = settings.DeveloperMode ? "1" : "0";
        var levels = settings.DeveloperMode
            ? WorldManifestLevelDiscovery.Discover(settings.GameDirectory, includeCampaign: true)
                .Where(l => l.CanCompile && !l.LevelDirectory.EndsWith("_PATCH", StringComparison.OrdinalIgnoreCase))
                .Select(l => Path.GetFileName(l.LevelDirectory)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray()
            : Array.Empty<string>();
        values["developerLevelCount"] = levels.Length.ToString(System.Globalization.CultureInfo.InvariantCulture);
        for (var i = 0; i < levels.Length; i++) values[$"developerLevel{i}"] = levels[i];
        return values;
    }
}
