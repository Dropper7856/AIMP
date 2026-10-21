using AIMP.Protocol;
using System.Text.Json;

namespace AIMP.Launcher;

internal sealed record LauncherSettings(
    string GameDirectory,
    string PlayerName = "Player",
    string HostAddress = "127.0.0.1",
    int Port = ProtocolConstants.DefaultGamePort,
    Guid PlayerId = default,
    bool WorldSyncEnabled = true,
    bool WorldSync3EmptyCallbackEnabled = false,
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


internal static class WorldSync3LauncherOptions
{
    internal const string EmptyCallbackEnvironmentVariable = "AIMP_WS3_EMPTY_CALLBACK";

    internal static void ApplyProcessEnvironment(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        Environment.SetEnvironmentVariable(
            EmptyCallbackEnvironmentVariable,
            settings.WorldSync3EmptyCallbackEnabled ? "1" : null,
            EnvironmentVariableTarget.Process);
    }
}

internal sealed class LauncherSettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly string _path;

    internal LauncherSettingsStore(string? path = null)
    {
        _path = path ?? DefaultPath();
    }

    internal string SettingsPath => _path;

    internal LauncherSettings Load()
    {
        if (!File.Exists(_path)) return LauncherSettings.Empty.Normalize();
        var json = File.ReadAllText(_path);
        var settings = JsonSerializer.Deserialize<LauncherSettings>(json, JsonOptions)
            ?? LauncherSettings.Empty;
        // System.Text.Json may supply default(bool) for a constructor
        // parameter missing from a legacy settings file. Preserve the pre-v13.3
        // behavior by treating an absent WorldSyncEnabled property as enabled.
        using var document = JsonDocument.Parse(json);
        if (!document.RootElement.TryGetProperty(nameof(LauncherSettings.WorldSyncEnabled), out _))
            settings = settings with { WorldSyncEnabled = true };
        return settings.Normalize();
    }

    internal void Save(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var directory = Path.GetDirectoryName(_path);
        if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
        File.WriteAllText(_path, JsonSerializer.Serialize(settings.Normalize(), JsonOptions));
    }

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
