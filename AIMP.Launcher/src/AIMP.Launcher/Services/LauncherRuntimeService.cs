using AIMP.Launcher.Contracts;
using AIMP.Protocol;

namespace AIMP.Launcher.Services;

public sealed class LauncherRuntimeService : BackgroundService
{
    private static readonly PerformanceIsolationModule[] DisableOrder =
    {
        PerformanceIsolationModule.LocalPlayerSampling,
        PerformanceIsolationModule.PositionNetworkTransmit,
        PerformanceIsolationModule.RemoteCharacterSpawn,
        PerformanceIsolationModule.EquipmentSync,
        PerformanceIsolationModule.ActionSync,
        PerformanceIsolationModule.RemoteMovement,
        PerformanceIsolationModule.AISync,
        PerformanceIsolationModule.WorldSync,
        PerformanceIsolationModule.UiBridge,
        PerformanceIsolationModule.EngineThreadEvidence,
        PerformanceIsolationModule.CharacterLifecycleCapture,
        PerformanceIsolationModule.Diagnostics,
        PerformanceIsolationModule.GameThreadPumps
    };

    private readonly NativeRuntimeController _controller;
    private readonly LauncherSettingsStore _settings;
    private readonly ScaleformBridgeListener _bridge;
    private readonly GameLaunchCoordinator _gameLauncher;
    private readonly MultiplayerSessionCoordinator _multiplayer;
    private readonly PerformanceIsolationState _performance;
    private readonly GamePreparationService _preparation;
    private readonly LocalPlayerPositionReader _positionReader = new();
    private readonly SemaphoreSlim _runtimeOperationGate = new(1, 1);
    private readonly SemaphoreSlim _performanceGate = new(1, 1);
    private LocalPlayerStatus _localPlayer = new(
        false, null, null, null, null, "Waiting for AI.exe", DateTimeOffset.UtcNow);
    private CancellationToken _stoppingToken;
    private Task _autoInjectionTask = Task.CompletedTask;
    private int _bridgeAutoInjectionRunning;
    private int _stopped;
    private int _disposed;

    internal LauncherRuntimeService(
        NativeRuntimeController controller,
        LauncherSettingsStore settings,
        ScaleformBridgeListener bridge,
        GameLaunchCoordinator gameLauncher,
        MultiplayerSessionCoordinator multiplayer,
        PerformanceIsolationState performance,
        GamePreparationService preparation)
    {
        _controller = controller;
        _settings = settings;
        _bridge = bridge;
        _gameLauncher = gameLauncher;
        _multiplayer = multiplayer;
        _performance = performance;
        _preparation = preparation;
        _bridge.StateChanged += OnBridgeStateChanged;
    }

    public LauncherStatusResponse GetStatus()
    {
        _controller.RefreshProcessStatus();
        var bridge = _bridge.State;
        var multiplayer = _multiplayer.State;
        return new LauncherStatusResponse(
            ProductInfo.LauncherVersion,
            MapRuntime(_controller.Snapshot),
            new BridgeStatus(bridge.Running, bridge.Connected, bridge.ConnectedClients),
            new MultiplayerStatus(
                multiplayer.NetworkState,
                multiplayer.IsHost,
                multiplayer.Connected,
                multiplayer.LocalGameplayReady,
                multiplayer.RemoteGameplayReady,
                multiplayer.RemotePlayerName),
            Volatile.Read(ref _localPlayer),
            GetPerformanceModules(),
            _controller.Diagnostics.Path);
    }

    public SettingsResponse GetSettings()
    {
        var value = LoadSettingsOrDefault();
        return MapSettings(value);
    }

    public SettingsValidationResponse ValidateSettings(UpdateSettingsRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!GameInstallation.TryResolve(request.GameDirectory, out var game, out var error))
            return new SettingsValidationResponse(false, error, null, null, null, null);
        if (!MultiplayerSettingsValidation.TryValidate(
                request.PlayerName, request.HostAddress, request.Port,
                out var playerName, out var host, out var port, out error))
            return new SettingsValidationResponse(false, error, game.GameDirectory, null, null, null);
        return new SettingsValidationResponse(
            true, string.Empty, game.GameDirectory, playerName, host, port);
    }

    public SettingsResponse SaveSettings(UpdateSettingsRequest request)
    {
        var validation = ValidateSettings(request);
        if (!validation.Valid) throw new ArgumentException(validation.Error, nameof(request));

        var previous = LoadSettingsOrDefault();
        if (_preparation.GetStatus().State == "Running"
            && !string.Equals(previous.GameDirectory, validation.NormalizedGameDirectory, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Wait for preparation to finish before changing the installation.");
        var value = new LauncherSettings(
            validation.NormalizedGameDirectory!,
            validation.NormalizedPlayerName!,
            validation.NormalizedHostAddress!,
            validation.NormalizedPort!.Value,
            previous.PlayerId == Guid.Empty ? Guid.NewGuid() : previous.PlayerId,
            request.WorldSyncEnabled,
            request.WorldSync3EmptyCallbackEnabled);
        _settings.Save(value);
        _preparation.Start();
        WorldSync3LauncherOptions.ApplyProcessEnvironment(value);
        _controller.Diagnostics.Log(
            "Settings",
            $"Settings saved for player {value.PlayerName}; " +
            $"WorldSync={(value.WorldSyncEnabled ? "enabled" : "disabled")}; " +
            $"WS3 ProbeDirect callback test={(value.WorldSync3EmptyCallbackEnabled ? "enabled" : "disabled")} for the next game session.");
        return MapSettings(value);
    }

    public async Task<GameLaunchResponse> LaunchGameAsync(CancellationToken cancellationToken)
    {
        await _runtimeOperationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var processId = await _gameLauncher.LaunchAsync(cancellationToken).ConfigureAwait(false);
            _controller.RefreshProcessStatus();
            return new GameLaunchResponse(processId);
        }
        finally
        {
            _runtimeOperationGate.Release();
        }
    }

    public async Task<InjectionResponse> InjectAsync(CancellationToken cancellationToken)
    {
        await _runtimeOperationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _controller.InjectAsync(cancellationToken).ConfigureAwait(false);
            return new InjectionResponse(MapRuntime(_controller.Snapshot));
        }
        finally
        {
            _runtimeOperationGate.Release();
        }
    }

    public async Task<DiagnosticsExportResponse> ExportDiagnosticsAsync(
        string? requestedPath,
        CancellationToken cancellationToken)
    {
        await _runtimeOperationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var path = string.IsNullOrWhiteSpace(requestedPath)
                ? DefaultDiagnosticsPath()
                : requestedPath.Trim();
            if (!path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) path += ".zip";
            var result = await _controller.ExportDiagnosticsAsync(path, cancellationToken)
                .ConfigureAwait(false);
            return new DiagnosticsExportResponse(result);
        }
        finally
        {
            _runtimeOperationGate.Release();
        }
    }

    public LogResponse GetLogs(int tail)
    {
        var entries = _controller.Diagnostics.SnapshotEntries();
        var count = Math.Clamp(tail, 1, 1_000);
        return new LogResponse(
            _controller.Diagnostics.Path,
            _controller.Diagnostics.Enabled,
            entries.Skip(Math.Max(0, entries.Count - count)).ToArray());
    }

    public IReadOnlyList<CommandDefinitionResponse> GetCommands() =>
        GameCommandCatalog.Commands.Select(MapCommand).ToArray();

    public async Task<CommandExecutionResponse> ExecuteCommandAsync(
        string commandName,
        IReadOnlyDictionary<string, string>? arguments,
        CancellationToken cancellationToken)
    {
        var command = GameCommandCatalog.Commands.FirstOrDefault(value =>
            string.Equals(value.Name, commandName, StringComparison.OrdinalIgnoreCase))
            ?? throw new ArgumentException($"Unknown command '{commandName}'.", nameof(commandName));
        IReadOnlyDictionary<string, string> normalized;
        try
        {
            normalized = GameCommandCatalog.ValidateAndNormalize(
                command,
                arguments ?? new Dictionary<string, string>(StringComparer.Ordinal));
        }
        catch (InvalidOperationException exception)
        {
            throw new ArgumentException(exception.Message, nameof(arguments), exception);
        }

        await _runtimeOperationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var result = command.Target == GameCommandExecutionTarget.Native
                ? await _controller.ExecuteCommandAsync(command.Name, normalized, cancellationToken)
                    .ConfigureAwait(false)
                : await _multiplayer.ExecuteLauncherCommandAsync(command.Name, normalized)
                    .WaitAsync(cancellationToken).ConfigureAwait(false);
            return new CommandExecutionResponse(
                result.RequestId,
                result.Command,
                result.Executed,
                result.StatusCode,
                result.Message,
                NativeRuntimeController.IsQueuedResult(result),
                NativeRuntimeController.CommandResultText(result));
        }
        finally
        {
            _runtimeOperationGate.Release();
        }
    }

    public IReadOnlyList<PerformanceModuleStatus> GetPerformanceModules() =>
        PerformanceIsolationCatalog.All
            .Select(definition => new PerformanceModuleStatus(
                definition.Module.ToString(),
                definition.Label,
                definition.Description,
                _performance.IsEnabled(definition.Module)))
            .ToArray();

    public async Task<PerformanceModuleStatus> SetPerformanceModuleAsync(
        string moduleName,
        bool enabled,
        CancellationToken cancellationToken)
    {
        if (!Enum.TryParse<PerformanceIsolationModule>(moduleName, true, out var module)
            || !Enum.IsDefined(module))
            throw new ArgumentException($"Unknown performance module '{moduleName}'.", nameof(moduleName));

        await _performanceGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await ApplyPerformanceModuleAsync(module, enabled, cancellationToken)
                .ConfigureAwait(false);
            var definition = PerformanceIsolationCatalog.Get(module);
            return new PerformanceModuleStatus(
                module.ToString(),
                definition.Label,
                definition.Description,
                _performance.IsEnabled(module));
        }
        finally
        {
            _performanceGate.Release();
        }
    }

    public async Task<IReadOnlyList<PerformanceModuleStatus>> SetAllPerformanceModulesAsync(
        bool enabled,
        CancellationToken cancellationToken)
    {
        await _performanceGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var order = enabled ? DisableOrder.Reverse() : DisableOrder;
            foreach (var module in order)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (_performance.IsEnabled(module) != enabled)
                    await ApplyPerformanceModuleAsync(module, enabled, cancellationToken)
                        .ConfigureAwait(false);
            }
            return GetPerformanceModules();
        }
        finally
        {
            _performanceGate.Release();
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _stoppingToken = stoppingToken;
        TryStartBridge();
        _controller.RefreshProcessStatus();

        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(16));
        var nextProcessRefresh = DateTimeOffset.UtcNow;
        try
        {
            while (await timer.WaitForNextTickAsync(stoppingToken).ConfigureAwait(false))
            {
                var now = DateTimeOffset.UtcNow;
                if (now >= nextProcessRefresh)
                {
                    _controller.RefreshProcessStatus();
                    nextProcessRefresh = now.AddSeconds(1);
                }
                PollLocalPlayer(now);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _stopped, 1) != 0) return;
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _autoInjectionTask.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        _bridge.StateChanged -= OnBridgeStateChanged;
        _multiplayer.Dispose();
        await _bridge.StopAsync().ConfigureAwait(false);
        await _controller.ShutdownAsync().ConfigureAwait(false);
    }

    public override void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _positionReader.Dispose();
        _runtimeOperationGate.Dispose();
        _performanceGate.Dispose();
        base.Dispose();
    }

    internal static NativeRuntimeStatus MapRuntime(NativeRuntimeSnapshot value) => new(
        value.AiRunning,
        value.ProcessId,
        value.ExecutablePath,
        $"0x{value.ModuleBase:X}",
        value.DllPath,
        value.DllInjected,
        value.IpcConnected,
        value.NativeDllVersion,
        value.EngineHooksInstalled,
        value.MainHookInstalled,
        value.FallbackHookInstalled,
        value.ProducerHookInstalled,
        value.EngineThreadReady,
        value.EngineThreadId,
        value.EngineThreadSource,
        value.FirstEngineCallbackUtc,
        value.LastEngineCallbackUtc,
        value.FirstEngineCallbackCounter,
        value.LastEngineCallbackCounter,
        value.PendingCommandCount,
        value.LastCommand,
        value.LastCommandResult,
        value.LastNativeException,
        value.LevelGeneration,
        value.NativeLogPath,
        new RemoteCharacterStatus(
            value.RemoteCharacterState,
            value.RemoteCharacterSpawnKey,
            $"0x{value.RemoteCharacterBase:X8}",
            $"0x{value.RemoteCharacterRecord:X8}",
            value.CharacterCreateHookInstalled));

    private async Task ApplyPerformanceModuleAsync(
        PerformanceIsolationModule module,
        bool enabled,
        CancellationToken cancellationToken)
    {
        switch (module)
        {
            case PerformanceIsolationModule.UiBridge:
                if (enabled) _bridge.Start();
                else await _bridge.StopAsync().ConfigureAwait(false);
                _performance.SetEnabled(module, enabled);
                break;

            case PerformanceIsolationModule.Diagnostics:
                if (enabled) _controller.Diagnostics.SetEnabled(true);
                try
                {
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.NativeDiagnostics,
                        enabled,
                        cancellationToken).ConfigureAwait(false);
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.LocomotionFlight,
                        enabled,
                        cancellationToken).ConfigureAwait(false);
                    _performance.SetEnabled(module, enabled);
                    if (!enabled) _controller.Diagnostics.SetEnabled(false);
                }
                catch
                {
                    _controller.Diagnostics.SetEnabled(true);
                    throw;
                }
                break;

            default:
                await _multiplayer.ApplyPerformanceIsolationModuleAsync(module, enabled)
                    .WaitAsync(cancellationToken).ConfigureAwait(false);
                break;
        }

        if (_controller.Diagnostics.Enabled)
            _controller.Diagnostics.Log(
                "Performance",
                $"Isolation module {module}={(enabled ? "ON" : "OFF")}.");
    }

    private void PollLocalPlayer(DateTimeOffset now)
    {
        var runtime = _controller.Snapshot;
        if (!_performance.IsEnabled(PerformanceIsolationModule.LocalPlayerSampling))
        {
            Volatile.Write(ref _localPlayer, new LocalPlayerStatus(
                false, null, null, null, null,
                "Disabled (Performance Isolation)", now));
            _multiplayer.UpdateLocalRuntime(
                runtime, false, default, false, default, false, default);
            return;
        }

        if (_positionReader.TryRead(runtime, out var position))
        {
            var yawAvailable = _positionReader.TryReadYaw(runtime, out var yaw);
            var movementAvailable =
                _positionReader.TryReadMovementProfile(runtime, out var movement);
            Volatile.Write(ref _localPlayer, new LocalPlayerStatus(
                true,
                position.X,
                position.Y,
                position.Z,
                yawAvailable ? yaw : null,
                position.ToDisplayText(yawAvailable ? yaw : null),
                now));
            _multiplayer.UpdateLocalRuntime(
                runtime,
                true,
                position,
                yawAvailable,
                yaw,
                movementAvailable,
                movement);
            return;
        }

        Volatile.Write(ref _localPlayer, new LocalPlayerStatus(
            false,
            null,
            null,
            null,
            null,
            runtime.AiRunning ? "Position unavailable" : "Waiting for AI.exe",
            now));
        _multiplayer.UpdateLocalRuntime(
            runtime, false, default, false, default, false, default);
    }

    private void TryStartBridge()
    {
        try
        {
            _bridge.Start();
        }
        catch (Exception exception)
        {
            _controller.Diagnostics.Log(
                "UI Bridge", $"Bridge startup failed: {exception.Message}");
        }
    }

    private void OnBridgeStateChanged(object? sender, ScaleformBridgeState state)
    {
        if (state.Connected && _autoInjectionTask.IsCompleted)
            _autoInjectionTask = EnsureBridgeAutoInjectionAsync();
    }

    private async Task EnsureBridgeAutoInjectionAsync()
    {
        var current = _controller.Snapshot;
        if ((current.DllInjected && current.IpcConnected)
            || Interlocked.CompareExchange(ref _bridgeAutoInjectionRunning, 1, 0) != 0)
            return;

        try
        {
            _controller.Diagnostics.Log(
                "Injection",
                "UI Bridge connected for the first time; automatic DLL injection is starting.");
            for (var attempt = 1; attempt <= 10 && !_stoppingToken.IsCancellationRequested; attempt++)
            {
                if (!_bridge.State.Connected) return;
                try
                {
                    await InjectAsync(_stoppingToken).ConfigureAwait(false);
                    current = _controller.Snapshot;
                    if (current.DllInjected && current.IpcConnected)
                    {
                        _controller.Diagnostics.Log(
                            "Injection",
                            "Automatic DLL injection/IPC handshake completed after UI Bridge connection.");
                        return;
                    }
                }
                catch (Exception exception) when (exception is not OperationCanceledException)
                {
                    _controller.Diagnostics.Log(
                        "Injection",
                        $"Automatic injection attempt {attempt}/10 failed: {exception.Message}");
                }
                await Task.Delay(500, _stoppingToken).ConfigureAwait(false);
            }
            _controller.Diagnostics.Log(
                "Injection",
                "Automatic injection did not complete while the UI Bridge remained connected; manual API injection remains available.");
        }
        catch (OperationCanceledException) when (_stoppingToken.IsCancellationRequested)
        {
        }
        finally
        {
            Volatile.Write(ref _bridgeAutoInjectionRunning, 0);
        }
    }

    private SettingsResponse MapSettings(LauncherSettings value) => new(
        value.GameDirectory,
        value.PlayerName,
        value.HostAddress,
        value.Port,
        value.PlayerId,
        value.WorldSyncEnabled,
        value.WorldSync3EmptyCallbackEnabled,
        _settings.SettingsPath);

    private LauncherSettings LoadSettingsOrDefault()
    {
        try
        {
            return _settings.Load();
        }
        catch (Exception exception)
        {
            _controller.Diagnostics.Log(
                "Settings",
                $"Settings load failed; using defaults until the next save: {exception.Message}");
            return LauncherSettings.Empty.Normalize();
        }
    }

    private static CommandDefinitionResponse MapCommand(GameCommandDefinition value) => new(
        value.Name,
        value.Label,
        value.Description,
        value.Target.ToString(),
        value.Parameters.Select(parameter => new CommandParameterResponse(
            parameter.Key,
            parameter.Label,
            parameter.Kind.ToString(),
            parameter.DefaultValue,
            parameter.Description,
            parameter.Minimum,
            parameter.Maximum,
            parameter.Increment,
            parameter.DecimalPlaces,
            (parameter.Choices ?? Array.Empty<GameCommandChoice>())
                .Select(choice => new CommandChoiceResponse(choice.Label, choice.Value))
                .ToArray(),
            parameter.MaximumUtf8Bytes)).ToArray());

    private static string DefaultDiagnosticsPath()
    {
        var electronRoot = Environment.GetEnvironmentVariable("AIMP_ELECTRON_ROOT");
        var outputDirectory = string.IsNullOrWhiteSpace(electronRoot)
            ? AppContext.BaseDirectory
            : Path.GetFullPath(electronRoot.Trim());
        return Path.Combine(
            outputDirectory,
            $"AIMP-Diagnostics-{DateTime.Now:yyyyMMdd-HHmmss}.zip");
    }
}
