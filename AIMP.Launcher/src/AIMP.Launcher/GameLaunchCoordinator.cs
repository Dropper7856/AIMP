using System.Diagnostics;

namespace AIMP.Launcher;

internal sealed class GameLaunchCoordinator
{
    private readonly LauncherSettingsStore _settings;
    private readonly UiPakInstaller _uiInstaller;
    private readonly ScaleformBridgeListener _bridge;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly GamePreparationService _preparation;

    internal GameLaunchCoordinator(
        LauncherSettingsStore settings,
        UiPakInstaller uiInstaller,
        ScaleformBridgeListener bridge,
        LauncherDiagnostics diagnostics,
        GamePreparationService preparation)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _uiInstaller = uiInstaller ?? throw new ArgumentNullException(nameof(uiInstaller));
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
        _preparation = preparation;
    }

    internal async Task<int> LaunchAsync(CancellationToken cancellationToken = default)
    {
        var settings = _settings.Load();
        var game = ResolveConfiguredGame(settings);
        EnsureGameIsNotRunning();
        await _preparation.EnsureReadyAsync(cancellationToken).ConfigureAwait(false);
        var latestSettings = _settings.Load();
        if (!string.Equals(settings.GameDirectory, latestSettings.GameDirectory, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The installation changed during preparation. Launch again.");
        settings = latestSettings;
        WorldSync3LauncherOptions.ApplyProcessEnvironment();
        EnsureGameIsNotRunning();
        _bridge.Start();
        return await Task.Run(() => InstallAndLaunch(game, settings), cancellationToken)
            .ConfigureAwait(false);
    }

    private static GameInstallationPaths ResolveConfiguredGame(LauncherSettings settings)
    {
        if (GameInstallation.TryResolve(settings.GameDirectory, out var game, out var error)) return game;
        throw new InvalidOperationException(error);
    }

    private int InstallAndLaunch(GameInstallationPaths game, LauncherSettings settings)
    {
        _diagnostics.Log("Launch", "Installing the multiplayer UI bridge into UI.PAK.");
        _uiInstaller.Install(game);
        _diagnostics.Log("Launch", "Launching Alien: Isolation.");
        using var process = Process.Start(CreateStartInfo(game))
            ?? throw new InvalidOperationException("AI.exe could not be started.");
        _diagnostics.Log("Launch", $"AI.exe started with PID={process.Id}; " +
            $"child {WorldSync3LauncherOptions.EmptyCallbackEnvironmentVariable}=1 " +
            "(ProbeDirect callback path; explicit process environment).");
        return process.Id;
    }

    private static ProcessStartInfo CreateStartInfo(GameInstallationPaths game)
    {
        var info = new ProcessStartInfo
        {
            FileName = game.ExecutablePath,
            WorkingDirectory = game.GameDirectory,
            UseShellExecute = false
        };
        // Both lobby roles use this launch path. Pin the permanent WS3 path in
        // the child's environment instead of relying on shell/global inheritance.
        info.Environment[WorldSync3LauncherOptions.EmptyCallbackEnvironmentVariable] = "1";
        return info;
    }

    private static void EnsureGameIsNotRunning()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            if (processes.Any(process => !process.HasExited))
                throw new InvalidOperationException("Alien: Isolation is already running.");
        }
        finally
        {
            foreach (var process in processes) process.Dispose();
        }
    }
}
