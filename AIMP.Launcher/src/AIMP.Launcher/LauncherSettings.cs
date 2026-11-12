using AIMP.Protocol;
using System.Text.Json;

namespace AIMP.Launcher;

internal sealed record LauncherSettings(
    string GameDirectory,
    string PlayerName = "Player",
    string HostAddress = "127.0.0.1",
    int Port = ProtocolConstants.DefaultGamePort,
    Guid PlayerId = default,
    VoiceSettings? Voice = null,
    bool DeveloperMode = false)
{
    internal static LauncherSettings Empty { get; } = new(string.Empty);

    internal LauncherSettings Normalize() => this with
    {
        Voice = (Voice ?? new VoiceSettings()).Normalize(),
        PlayerName = NormalizeName(PlayerName),
        HostAddress = NormalizeHost(HostAddress),
        Port = Port is >= 1 and <= 65535 ? Port : ProtocolConstants.DefaultGamePort,
        PlayerId = PlayerId == Guid.Empty ? Guid.NewGuid() : PlayerId
    };

    private static string NormalizeName(string? value) =>
        AIMP.Protocol.PlayerName.TryNormalize(value, out var normalized, out _) ? normalized : "Player";

    private static string NormalizeHost(string? value) =>
        string.IsNullOrWhiteSpace(value) ? "127.0.0.1" : value.Trim();
}


// WorldSync and the WS3 ProbeDirect callback path are permanent in public
// builds; neither is a user setting.
internal static class WorldSync3LauncherOptions
{
    internal const string EmptyCallbackEnvironmentVariable = "AIMP_WS3_EMPTY_CALLBACK";
    internal const bool EmptyCallbackEnabled = true;

    internal static void ApplyProcessEnvironment() =>
        Environment.SetEnvironmentVariable(
            EmptyCallbackEnvironmentVariable, "1", EnvironmentVariableTarget.Process);
}

internal sealed class LauncherSettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly string _path;
    private readonly Func<string?>? _steamPersona;

    // steamPersona supplies the Steam display name. When it returns a name, it
    // is the player name; the stored name is only a fallback without Steam.
    internal LauncherSettingsStore(string? path = null, bool developerModeAvailable = false,
        Func<string?>? steamPersona = null)
    {
        _path = path ?? DefaultPath();
        DeveloperModeAvailable = developerModeAvailable;
        _steamPersona = steamPersona;
    }

    // Each launcher instance (split-screen) has its own settings and player
    // identity. A new instance starts from the default instance's settings.
    internal static LauncherSettingsStore ForInstance(bool developerModeAvailable, Func<string?> steamPersona)
    {
        var slot = LauncherInstances.Slot;
        if (slot == 1) return new LauncherSettingsStore(developerModeAvailable: developerModeAvailable, steamPersona: steamPersona);
        var primary = new LauncherSettingsStore(developerModeAvailable: developerModeAvailable);
        var store = new LauncherSettingsStore(
            Path.Combine(Path.GetDirectoryName(DefaultPath())!, LauncherInstances.SlotFileName(Path.GetFileName(DefaultPath()), slot)),
            developerModeAvailable, () => steamPersona() is { } name ? InstanceName(name, slot) : null);
        if (!File.Exists(store.SettingsPath) && File.Exists(primary.SettingsPath))
        {
            try
            {
                var seed = primary.Load();
                store.Save(seed with { PlayerName = InstanceName(seed.PlayerName, slot), PlayerId = Guid.NewGuid() });
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException) { }
        }
        return store;
    }

    // Both players share one Steam account: tell them apart in the lobby.
    internal static string InstanceName(string name, int slot)
    {
        var suffix = $" ({slot})";
        var limit = ProtocolConstants.MaximumPlayerNameLength - suffix.Length;
        return (name.Length > limit ? name[..limit].TrimEnd() : name) + suffix;
    }

    internal string SettingsPath => _path;
    internal bool DeveloperModeAvailable { get; }

    internal string? SteamPersonaName() => _steamPersona?.Invoke();

    internal LauncherSettings Load()
    {
        if (!File.Exists(_path)) return ForSession(LauncherSettings.Empty);
        var json = File.ReadAllText(_path);
        var settings = JsonSerializer.Deserialize<LauncherSettings>(json, JsonOptions)
            ?? LauncherSettings.Empty;
        return ForSession(settings);
    }

    internal void Save(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var directory = Path.GetDirectoryName(_path);
        if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
        File.WriteAllText(_path, JsonSerializer.Serialize(ForSession(settings), JsonOptions));
    }

    internal void SetDeveloperMode(bool enabled)
    {
        if (!DeveloperModeAvailable)
            throw new InvalidOperationException("Restart the launcher with -dev to access Developer mode.");
        Save(Load() with { DeveloperMode = enabled });
    }

    private LauncherSettings ForSession(LauncherSettings settings) =>
        (settings with
        {
            DeveloperMode = DeveloperModeAvailable && settings.DeveloperMode,
            PlayerName = SteamPersonaName() ?? settings.PlayerName
        }).Normalize();

    private static string DefaultPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AIMP",
        "launcher-settings.json");
}

internal static class MultiplayerSettingsValidation
{
    internal static bool TryValidate(
        string? playerName,
        string? hostAddress,
        decimal port,
        out string normalizedName,
        out string normalizedHost,
        out int normalizedPort,
        out string error)
    {
        normalizedName = string.Empty;
        normalizedHost = string.Empty;
        normalizedPort = 0;
        if (!PlayerName.TryNormalize(playerName, out normalizedName, out error)) return false;
        if (!TryHost(hostAddress, out normalizedHost, out error)) return false;
        if (port is < 1 or > 65535) return Fail("Port must be between 1 and 65535.", out error);
        normalizedPort = decimal.ToInt32(port);
        error = string.Empty;
        return true;
    }

    private static bool TryHost(string? value, out string host, out string error)
    {
        host = value?.Trim() ?? string.Empty;
        if (host.Length is >= 1 and <= 253 && !host.Any(char.IsWhiteSpace))
        { error = string.Empty; return true; }
        return Fail("Server IP or host name is invalid.", out error);
    }

    private static bool Fail(string message, out string error)
    {
        error = message;
        return false;
    }
}

internal sealed record GameInstallationPaths(
    string GameDirectory,
    string ExecutablePath,
    string UiPakPath)
{
    internal static GameInstallationPaths Empty { get; } = new(
        string.Empty, string.Empty, string.Empty);
}

internal static class GameInstallation
{
    internal static bool TryResolve(
        string? gameDirectory,
        out GameInstallationPaths paths,
        out string error)
    {
        paths = GameInstallationPaths.Empty;
        if (string.IsNullOrWhiteSpace(gameDirectory))
            return Fail("Select the Alien: Isolation game directory.", out error);
        try { paths = Resolve(gameDirectory); }
        catch (Exception exception) when (exception is ArgumentException or NotSupportedException)
        { return Fail(exception.Message, out error); }
        return Validate(paths, out error);
    }

    internal static GameInstallationPaths Resolve(string gameDirectory)
    {
        var root = Path.GetFullPath(gameDirectory.Trim());
        return new GameInstallationPaths(root, Path.Combine(root, "AI.exe"),
            Path.Combine(root, "DATA", "UI.PAK"));
    }

    private static bool Validate(GameInstallationPaths paths, out string error)
    {
        if (!Directory.Exists(paths.GameDirectory))
            return Fail("The selected game directory does not exist.", out error);
        if (!File.Exists(paths.ExecutablePath))
            return Fail("AI.exe was not found in the selected directory.", out error);
        if (!File.Exists(paths.UiPakPath))
            return Fail("DATA\\UI.PAK was not found in the selected directory.", out error);
        error = string.Empty;
        return true;
    }

    private static bool Fail(string message, out string error)
    {
        error = message;
        return false;
    }
}
