using AIMP.Protocol;
using System.Collections.Concurrent;
using System.Diagnostics;

namespace AIMP.Launcher;

internal sealed class NativeRuntimeController : IDisposable
{
    private readonly LauncherDiagnostics _diagnostics;
    private readonly LauncherSettingsStore _settings;
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    private readonly object _stateGate = new();
    private readonly object _commandResultGate = new();
    private readonly Guid _runtimeIdentity = Guid.NewGuid();
    private readonly ConcurrentDictionary<Guid, byte> _awaitingCommandAcks = new();
    private readonly ConcurrentDictionary<Guid, byte> _earlyFinalCommands = new();
    private readonly ConcurrentDictionary<Guid, byte> _quietCommandIds = new();
    private NativeRuntimeSnapshot _snapshot = NativeRuntimeSnapshot.Empty;
    private Process? _process;
    private DllIpcSession? _ipc;
    private int _disposed;

    internal NativeRuntimeController(LauncherDiagnostics diagnostics, LauncherSettingsStore? settings = null)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
        _settings = settings ?? new LauncherSettingsStore();
        _diagnostics.Log("Launcher", $"AIMP Launcher {ProductInfo.LauncherVersion} started.");
    }

    internal NativeRuntimeSnapshot Snapshot
    {
        get { lock (_stateGate) return _snapshot; }
    }

    internal LauncherDiagnostics Diagnostics => _diagnostics;

    internal event EventHandler<NativeRuntimeSnapshot>? StateChanged;

    internal event EventHandler<WorldIntent>? WorldIntentProduced;

    internal event EventHandler<WorldMutationResult>? WorldMutationCompleted;

    internal event EventHandler<NativePlayerEquipmentStatePayload>? PlayerSyncStateProduced;

    internal event EventHandler<NativePlayerActionEventPayload>? PlayerSyncActionProduced;

    internal void RefreshProcessStatus()
    {
        if (Volatile.Read(ref _disposed) != 0 || HasTrackedProcess()) return;
        try
        {
            UpdateProcessPresence(AiProcessDiscovery.Find());
        }
        catch (InvalidOperationException)
        {
            MarkProcessAbsent();
        }
    }

    internal async Task InjectAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await InjectCoreAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            await HandleInjectionFailureAsync(exception).ConfigureAwait(false);
            throw;
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    private async Task InjectCoreAsync(CancellationToken token)
    {
        Log("Searching for AI.exe");
        var target = AiProcessDiscovery.Find();
        Log($"Found AI.exe PID={target.ProcessId}");
        await AttachProcessAsync(target).ConfigureAwait(false);
        var dllPath = NativeDllResolver.Resolve(AppContext.BaseDirectory);
        Update(value => value with { DllPath = dllPath });
        Log($"DLL resolved: {dllPath}");
        if (IsConnectedTo(target)) { Log("DLL is already connected; injection skipped."); return; }
        var session = await ReplaceSessionAsync(target.ProcessId).ConfigureAwait(false);
        await EnsureNativeDllLoadedAsync(target, dllPath, token).ConfigureAwait(false);
        await CompleteIpcConnectionAsync(session, target, token).ConfigureAwait(false);
    }

    private async Task CompleteIpcConnectionAsync(
        DllIpcSession session, AiProcessTarget target, CancellationToken token)
    {
        var handshake = await session.AcceptAndHandshakeAsync(target.ExecutablePath,
            _runtimeIdentity, TimeSpan.FromSeconds(20), token).ConfigureAwait(false);
        // Session validates the connecting Native PID. Configure that instance,
        // including reconnects, before exposing it as ready to gameplay callers.
        await SynchronizeWorldSync3OptionsAsync(session, target.ProcessId, token).ConfigureAwait(false);
        CompleteHandshake(handshake);
        await QueryCapabilitiesAsync(session, token).ConfigureAwait(false);
    }

    private async Task SynchronizeWorldSync3OptionsAsync(
        DllIpcSession session, int processId, CancellationToken token)
    {
        var enabled = _settings.Load().WorldSync3EmptyCallbackEnabled;
        var request = CreateWorldSync3OptionsRequest(enabled);
        Log($"Applying WS3 ProbeDirect callback test={(enabled ? 1 : 0)} via IPC to AI.exe PID={processId}.");
        var result = await session.ExecuteGameCommandAsync(request,
            TimeSpan.FromSeconds(5), token).ConfigureAwait(false);
        if (!result.Executed || result.Command != request.Command
            || result.StatusCode != "WS3_EMPTY_CALLBACK_APPLIED"
            || result.Message != $"ws3EmptyCallbackTest={(enabled ? 1 : 0)}")
            throw new ProtocolException($"Native WS3 runtime setting was not acknowledged: {result.StatusCode}.");
        Log($"Native AI.exe PID={processId} acknowledged {result.Message}; source=launcher-ipc.");
    }

    private GameCommandExecutePayload CreateWorldSync3OptionsRequest(bool enabled) => new(
        ProtocolConstants.ExperimentalSchemaVersion, Guid.NewGuid(), 0, _runtimeIdentity,
        GameCommandNames.WorldWs3EmptyCallbackSet,
        new Dictionary<string, string> { ["enabled"] = enabled ? "1" : "0" });

    private async Task AttachProcessAsync(AiProcessTarget target)
    {
        if (IsTrackedTarget(target)) { ApplyProcessTarget(target); return; }
        await CloseSessionAsync().ConfigureAwait(false);
        DetachProcess();
        var process = Process.GetProcessById(target.ProcessId);
        process.EnableRaisingEvents = true;
        process.Exited += OnProcessExited;
        _process = process;
        ResetForNewProcess(target);
    }

    private bool IsTrackedTarget(AiProcessTarget target)
    {
        try
        {
            return _process is { HasExited: false }
                && _process.Id == target.ProcessId
                && _process.StartTime.ToUniversalTime().Ticks == target.StartTimeUtcTicks;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private void ApplyProcessTarget(AiProcessTarget target)
    {
        Update(value => value with
        {
            AiRunning = true,
            ProcessId = target.ProcessId,
            ExecutablePath = target.ExecutablePath,
            ModuleBase = target.ModuleBase
        });
    }

    private void ResetForNewProcess(AiProcessTarget target)
    {
        var previous = Snapshot;
        Update(_ => NativeRuntimeSnapshot.Empty with
        {
            AiRunning = true, ProcessId = target.ProcessId,
            ExecutablePath = target.ExecutablePath, ModuleBase = target.ModuleBase,
            DllPath = previous.DllPath, LastCommand = previous.LastCommand,
            LastCommandResult = previous.LastCommandResult
        });
    }

    private bool IsConnectedTo(AiProcessTarget target) =>
        _ipc is { IsReady: true }
        && Snapshot.ProcessId == target.ProcessId
        && Snapshot.IpcConnected;

    private async Task<DllIpcSession> ReplaceSessionAsync(int processId)
    {
        await CloseSessionAsync().ConfigureAwait(false);
        // The pipe exists before module inspection so an existing DLL can reconnect safely.
        var session = new DllIpcSession(processId);
        SubscribeSession(session);
        _ipc = session;
        return session;
    }

    private async Task EnsureNativeDllLoadedAsync(
        AiProcessTarget target,
        string dllPath,
        CancellationToken token)
    {
        if (NativeModuleInspector.IsLoaded(target.ProcessId, dllPath))
        {
            Log("Native DLL is already loaded; reconnecting without reinjection.");
        }
        else
        {
            await InjectNativeDllAsync(target, dllPath, token).ConfigureAwait(false);
        }

        Update(value => value with { DllInjected = true });
    }

    private async Task InjectNativeDllAsync(
        AiProcessTarget target,
        string dllPath,
        CancellationToken token)
    {
        var injector = InjectorPathResolver.Resolve(AppContext.BaseDirectory);
        Log("Injecting DLL");
        var result = await InjectorRunner.RunAsync(injector, target, dllPath, token)
            .ConfigureAwait(false);
        if (!result.Success && NativeModuleInspector.IsLoaded(target.ProcessId, dllPath))
        {
            Log($"DLL became loaded during injection ({result.Code}); reconnecting.");
            return;
        }
        if (!result.Success) throw new InvalidOperationException($"Injection failed ({result.Code}).");
        Log($"DLL injected ({result.Code})");
    }

    private void CompleteHandshake(RuntimeHandshakeResult handshake)
    {
        Update(value => value with
        {
            DllInjected = true,
            IpcConnected = true,
            NativeDllVersion = handshake.NativeDllVersion,
            LastNativeException = string.Empty
        });
        Log("IPC connected");
    }

    private async Task QueryCapabilitiesAsync(DllIpcSession session, CancellationToken token)
    {
        try
        {
            var request = new CapabilityQueryPayload(ProtocolConstants.ExperimentalSchemaVersion,
                Guid.NewGuid(), Snapshot.LevelGeneration, _runtimeIdentity);
            var state = await session.QueryCapabilitiesAsync(request,
                TimeSpan.FromSeconds(5), token).ConfigureAwait(false);
            ApplyCapabilityState(state);
        }
        catch (Exception exception) when (exception is ProtocolException or TimeoutException)
        {
            Log($"Capability query did not complete: {exception.Message}");
        }
    }

    internal async Task<GameCommandResultPayload> ExecuteCommandAsync(
        string command,
        IReadOnlyDictionary<string, string> arguments,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        var request = CreateCommandRequest(command, arguments);
        _awaitingCommandAcks.TryAdd(request.RequestId, 0);
        BeginCommand(command);
        return await AwaitCommandAsync(session, request, cancellationToken).ConfigureAwait(false);
    }

    internal async Task<GameCommandResultPayload> ExecuteReplicationCommandAsync(
        string command,
        IReadOnlyDictionary<string, string> arguments,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        var request = CreateCommandRequest(command, arguments);
        _quietCommandIds.TryAdd(request.RequestId, 0);
        try
        {
            var result = await session.ExecuteGameCommandAsync(request, TimeSpan.FromSeconds(5),
                cancellationToken).ConfigureAwait(false);
            if (!IsQueuedResult(result)) _quietCommandIds.TryRemove(request.RequestId, out _);
            return result;
        }
        catch
        {
            _quietCommandIds.TryRemove(request.RequestId, out _);
            throw;
        }
    }

    internal ulong ReserveWorldMutationDispatchId()
    {
        var session = GetCommandSession();
        return session.ReserveWorldMutationDispatchId();
    }

    internal Task SendWorldSyncLifecycleAsync(
        WorldSyncLifecyclePayload lifecycle,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        return session.SendWorldSyncLifecycleAsync(lifecycle, cancellationToken);
    }

    internal Task SendWorldMutationAsync(
        WorldMutation mutation,
        ulong nativeDispatchId,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        return session.SendWorldMutationAsync(mutation, nativeDispatchId, cancellationToken);
    }

    internal Task SendWorldIntentRejectedAsync(
        WorldIntent intent,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        return session.SendWorldIntentRejectedAsync(intent, cancellationToken);
    }

    internal Task SendWorldIntentAbandonedAsync(
        WorldIntent intent,
        CancellationToken cancellationToken = default)
    {
        var session = GetCommandSession();
        return session.SendWorldIntentAbandonedAsync(intent, cancellationToken);
    }

    private async Task<GameCommandResultPayload> AwaitCommandAsync(
        DllIpcSession session,
        GameCommandExecutePayload request,
        CancellationToken cancellationToken)
    {
        try
        {
            var result = await session.ExecuteGameCommandAsync(request,
                TimeSpan.FromSeconds(20), cancellationToken).ConfigureAwait(false);
            CompleteCommandResponse(result);
            return result;
        }
        catch (Exception exception)
        {
            FailSubmittedCommand(request.RequestId, exception);
            throw;
        }
    }

    private DllIpcSession GetCommandSession()
    {
        RefreshProcessStatus();
        var state = Snapshot;
        if (!state.AiRunning) throw new InvalidOperationException("AI.exe is not running");
        if (!state.DllInjected) throw new InvalidOperationException("DLL not injected");
        if (!state.IpcConnected || _ipc is not { IsReady: true })
            throw new InvalidOperationException("DLL IPC is not connected");
        return _ipc;
    }

    private GameCommandExecutePayload CreateCommandRequest(
        string command,
        IReadOnlyDictionary<string, string> arguments) => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        Guid.NewGuid(),
        Snapshot.LevelGeneration,
        _runtimeIdentity,
        command,
        arguments);

    private void BeginCommand(string command)
    {
        var waiting = !Snapshot.EngineThreadReady;
        Update(value => value with
        {
            PendingCommandCount = value.PendingCommandCount + 1,
            LastCommand = command,
            LastCommandResult = waiting ? "Command queued" : "Command submitted"
        });
        Log($"Command submitted: {command}");
    }

    private void FinishCommand(GameCommandResultPayload result)
    {
        var queued = IsQueuedResult(result);
        var text = CommandResultText(result);
        Update(value => value with
        {
            PendingCommandCount = queued ? value.PendingCommandCount
                : Math.Max(0, value.PendingCommandCount - 1),
            LastCommandResult = text,
            LevelGeneration = result.LevelGeneration
        });
        Log(text);
    }

    private void CompleteCommandResponse(GameCommandResultPayload result)
    {
        lock (_commandResultGate)
        {
            _awaitingCommandAcks.TryRemove(result.RequestId, out _);
            if (IsQueuedResult(result)
                && _earlyFinalCommands.TryRemove(result.RequestId, out _)) return;
            FinishCommand(result);
        }
    }

    private void FailSubmittedCommand(Guid requestId, Exception exception)
    {
        lock (_commandResultGate)
        {
            _awaitingCommandAcks.TryRemove(requestId, out _);
            _earlyFinalCommands.TryRemove(requestId, out _);
            FailCommand(exception);
        }
    }

    private void FailCommand(Exception exception)
    {
        Update(value => value with
        {
            PendingCommandCount = Math.Max(0, value.PendingCommandCount - 1),
            LastCommandResult = $"Command failed: {exception.Message}",
            LastNativeException = exception.Message
        });
        Log($"Command failed: {exception.Message}");
    }

    internal static bool IsQueuedResult(GameCommandResultPayload result) =>
        string.Equals(result.StatusCode, "QUEUED_WAITING_FOR_ENGINE_THREAD",
            StringComparison.Ordinal);

    internal static string CommandResultText(GameCommandResultPayload result)
    {
        if (IsQueuedResult(result)) return "Command queued";
        var outcome = result.Executed ? "Command completed" : "Command rejected";
        var detail = string.IsNullOrWhiteSpace(result.Message)
            ? "The native runtime supplied no additional detail." : result.Message.Trim();
        var text = $"{outcome} ({result.StatusCode}): {detail}";
        return string.Equals(result.StatusCode, "INVALID_COMMAND", StringComparison.Ordinal)
            ? text + " Engine-thread readiness is not the blocker." : text;
    }

    internal async Task<string> ExportDiagnosticsAsync(
        string archivePath,
        CancellationToken cancellationToken = default)
    {
        var native = await TryRequestNativeDiagnosticsAsync(cancellationToken)
            .ConfigureAwait(false);
        _diagnostics.Flush();
        var result = await RuntimeDiagnosticsExporter.ExportAsync(archivePath, Snapshot,
            _diagnostics, native, cancellationToken).ConfigureAwait(false);
        Log($"Diagnostics exported: {result}");
        return result;
    }

    private async Task<DiagnosticSnapshotResultPayload?> TryRequestNativeDiagnosticsAsync(
        CancellationToken token)
    {
        if (_ipc is not { IsReady: true } session) return null;
        try
        {
            var request = CreateDiagnosticRequest();
            var result = await session.RequestDiagnosticsAsync(request,
                TimeSpan.FromSeconds(10), token).ConfigureAwait(false);
            Update(value => value with { NativeLogPath = result.NativeLogPath });
            return result;
        }
        catch (Exception exception)
        {
            Log($"Native diagnostics unavailable: {exception.Message}");
            return null;
        }
    }

    private DiagnosticSnapshotRequestPayload CreateDiagnosticRequest() => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        Guid.NewGuid(),
        Snapshot.LevelGeneration,
        _runtimeIdentity,
        true);

    private void SubscribeSession(DllIpcSession session)
    {
        session.Log += OnSessionLog;
        session.CapabilityChanged += OnCapabilityChanged;
        session.CaptureChanged += OnCaptureChanged;
        session.LevelStateChanged += OnLevelStateChanged;
        session.NativeError += OnNativeError;
        session.GameCommandResultReceived += OnFinalCommandResult;
        session.WorldIntentReceived += OnWorldIntentReceived;
        session.WorldMutationResultReceived += OnWorldMutationResultReceived;
        session.PlayerSyncStateChanged += OnPlayerSyncStateChanged;
        session.PlayerSyncActionProduced += OnPlayerSyncActionProduced;
        session.ConnectionClosed += OnConnectionClosed;
    }

    private void UnsubscribeSession(DllIpcSession session)
    {
        session.Log -= OnSessionLog;
        session.CapabilityChanged -= OnCapabilityChanged;
        session.CaptureChanged -= OnCaptureChanged;
        session.LevelStateChanged -= OnLevelStateChanged;
        session.NativeError -= OnNativeError;
        session.GameCommandResultReceived -= OnFinalCommandResult;
        session.WorldIntentReceived -= OnWorldIntentReceived;
        session.WorldMutationResultReceived -= OnWorldMutationResultReceived;
        session.PlayerSyncStateChanged -= OnPlayerSyncStateChanged;
        session.PlayerSyncActionProduced -= OnPlayerSyncActionProduced;
        session.ConnectionClosed -= OnConnectionClosed;
    }

    private void OnSessionLog(object? sender, string message) => Log(message, "DLL");

    private void OnWorldIntentReceived(object? sender, WorldIntent value) =>
        WorldIntentProduced?.Invoke(this, value);

    private void OnWorldMutationResultReceived(object? sender, WorldMutationResult value) =>
        WorldMutationCompleted?.Invoke(this, value);

    private void OnPlayerSyncStateChanged(
        object? sender,
        NativePlayerEquipmentStatePayload value) =>
        PlayerSyncStateProduced?.Invoke(this, value);

    private void OnPlayerSyncActionProduced(
        object? sender,
        NativePlayerActionEventPayload value) =>
        PlayerSyncActionProduced?.Invoke(this, value);


    private void OnCapabilityChanged(object? sender, CapabilityStatePayload value) =>
        ApplyCapabilityState(value);

    private void ApplyCapabilityState(CapabilityStatePayload value)
    {
        var capability = value.Capabilities.FirstOrDefault(item => string.Equals(item.Name,
            NativeCapabilityNames.ScheduleGameThreadWork, StringComparison.Ordinal));
        if (capability is null) return;
        var current = Snapshot;
        var installed = IsHookCapabilityInstalled(capability.State);
        var validated = string.Equals(capability.State, CapabilityStateNames.RuntimeValidated,
            StringComparison.Ordinal);
        var ready = validated && current.EngineThreadId > 0;
        var source = validated ? ParseEngineThreadSource(capability.Reason) : current.EngineThreadSource;
        var update = new EngineStateUpdate(installed, current.MainHookInstalled,
            current.FallbackHookInstalled, current.ProducerHookInstalled, ready,
            current.EngineThreadId, source, current.FirstEngineCallbackCounter,
            current.LastEngineCallbackCounter, current.PendingCommandCount, value.LevelGeneration);
        ApplyEngineState(update);
    }

    private static bool IsHookCapabilityInstalled(string state) =>
        string.Equals(state, CapabilityStateNames.Observed, StringComparison.Ordinal)
        || string.Equals(state, CapabilityStateNames.RuntimeValidated, StringComparison.Ordinal)
        || string.Equals(state, CapabilityStateNames.StaticValidated, StringComparison.Ordinal);

    private static string ParseEngineThreadSource(string reason)
    {
        const string marker = "engine_thread_bound_";
        var index = reason.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
        if (index < 0) return "Engine callback";
        return NormalizeEngineThreadSource(reason[(index + marker.Length)..]);
    }

    private static string NormalizeEngineThreadSource(string source)
    {
        if (source.Contains("MainThreadCharacterProcessing", StringComparison.OrdinalIgnoreCase))
            return "MainThreadCharacterProcessing";
        if (source.Contains("CharacterManagerUpdate", StringComparison.OrdinalIgnoreCase))
            return "CharacterManagerUpdate";
        return source.Trim('_', ' ');
    }

    private void OnCaptureChanged(object? sender, CaptureStatePayload value)
    {
        ApplyRemoteCharacterState(value);
        var threadId = value.EngineThreadId > 0
            ? value.EngineThreadId : value.GameThreadCandidateId;
        var source = NormalizeCaptureSource(value.EngineThreadSource);
        var update = new EngineStateUpdate(value.EngineHooksInstalled,
            value.MainThreadCharacterProcessingHookInstalled,
            value.CharacterManagerUpdateHookInstalled,
            value.CharacterReadinessProducerHookInstalled, value.GameThreadValidated,
            threadId, source, value.FirstEngineCallbackPerformanceCounter,
            value.LastEngineCallbackPerformanceCounter, value.PendingCommandCount,
            value.LevelGeneration);
        ApplyEngineState(update);
    }

    private void ApplyRemoteCharacterState(CaptureStatePayload value)
    {
        var before = Snapshot;
        Update(state => state with
        {
            RemoteCharacterState = value.RemoteCharacterState,
            RemoteCharacterSpawnKey = value.RemoteCharacterSpawnKey,
            RemoteCharacterBase = (uint)value.RemoteCharacterAddress,
            RemoteCharacterRecord = (uint)value.RemoteCharacterRecordAddress,
            CharacterCreateHookInstalled = value.CharacterCreateHookInstalled
        });
        if (RemoteCharacterChanged(before, Snapshot)) Log(RemoteCharacterText(Snapshot), "DLL");
    }

    private static bool RemoteCharacterChanged(
        NativeRuntimeSnapshot before,
        NativeRuntimeSnapshot after) =>
        before.RemoteCharacterState != after.RemoteCharacterState
        || before.RemoteCharacterSpawnKey != after.RemoteCharacterSpawnKey
        || before.RemoteCharacterBase != after.RemoteCharacterBase;

    private static string RemoteCharacterText(NativeRuntimeSnapshot value) =>
        value.RemoteCharacterState == RemoteCharacterStateNames.Ready
            ? $"Remote Character ready: CharacterBase=0x{value.RemoteCharacterBase:X8}, spawnKey={value.RemoteCharacterSpawnKey}."
            : $"Remote Character state: {value.RemoteCharacterState}, spawnKey={value.RemoteCharacterSpawnKey}.";

    private void OnLevelStateChanged(object? sender, LevelStatePayload value)
    {
        var current = Snapshot;
        var ready = value.GameThreadValidated && current.EngineThreadId > 0;
        var update = new EngineStateUpdate(current.EngineHooksInstalled,
            current.MainHookInstalled, current.FallbackHookInstalled,
            current.ProducerHookInstalled,
            ready, current.EngineThreadId, current.EngineThreadSource,
            current.FirstEngineCallbackCounter, current.LastEngineCallbackCounter,
            current.PendingCommandCount, value.LevelGeneration);
        ApplyEngineState(update);
    }

    private string NormalizeCaptureSource(string source)
    {
        if (!string.Equals(source, "Unknown", StringComparison.OrdinalIgnoreCase)) return source;
        return Snapshot.EngineThreadSource;
    }

    private void ApplyEngineState(EngineStateUpdate state)
    {
        var wasReady = Snapshot.EngineThreadReady;
        Update(value => MergeEngineState(
            ResetRemoteCharacterForGeneration(value, state.Generation), state));
        if (state.Ready && !wasReady)
            Log($"Engine thread ready: {state.ThreadId} ({state.Source})");
    }

    private static NativeRuntimeSnapshot ResetRemoteCharacterForGeneration(
        NativeRuntimeSnapshot value,
        int generation) => generation == value.LevelGeneration ? value : value with
    {
        RemoteCharacterState = RemoteCharacterStateNames.None,
        RemoteCharacterSpawnKey = 0,
        RemoteCharacterBase = 0,
        RemoteCharacterRecord = 0
    };

    private static NativeRuntimeSnapshot MergeEngineState(
        NativeRuntimeSnapshot value,
        EngineStateUpdate state)
    {
        var now = DateTimeOffset.UtcNow;
        var first = ResolveFirstCallbackTime(value, state, now);
        var last = ResolveLastCallbackTime(value, state, now);
        return value with
        {
            EngineHooksInstalled = state.HooksInstalled, MainHookInstalled = state.MainHookInstalled,
            FallbackHookInstalled = state.FallbackHookInstalled,
            ProducerHookInstalled = state.ProducerHookInstalled, EngineThreadReady = state.Ready,
            EngineThreadId = state.ThreadId, EngineThreadSource = state.Source,
            PendingCommandCount = state.PendingCount, LevelGeneration = state.Generation,
            FirstEngineCallbackUtc = first, LastEngineCallbackUtc = last,
            FirstEngineCallbackCounter = state.FirstCallbackCounter,
            LastEngineCallbackCounter = state.LastCallbackCounter
        };
    }

    private static DateTimeOffset? ResolveFirstCallbackTime(
        NativeRuntimeSnapshot value,
        EngineStateUpdate state,
        DateTimeOffset now) => state.FirstCallbackCounter > 0
        && state.FirstCallbackCounter != value.FirstEngineCallbackCounter
            ? value.FirstEngineCallbackUtc ?? now : value.FirstEngineCallbackUtc;

    private static DateTimeOffset? ResolveLastCallbackTime(
        NativeRuntimeSnapshot value,
        EngineStateUpdate state,
        DateTimeOffset now) => state.LastCallbackCounter > 0
        && state.LastCallbackCounter != value.LastEngineCallbackCounter
            ? now : value.LastEngineCallbackUtc;


    private void OnNativeError(object? sender, ExperimentalErrorPayload value)
    {
        Update(state => state with { LastNativeException = value.Code });
        Log($"Native error: {value.Code}", "DLL");
    }

    private void OnFinalCommandResult(object? sender, GameCommandResultPayload value)
    {
        if (_quietCommandIds.TryRemove(value.RequestId, out _)) return;
        lock (_commandResultGate)
        {
            if (_awaitingCommandAcks.ContainsKey(value.RequestId))
                _earlyFinalCommands.TryAdd(value.RequestId, 0);
            FinishCommand(value);
        }
    }

    private void OnConnectionClosed(object? sender, EventArgs value)
    {
        Update(state => state with
        {
            IpcConnected = false,
            EngineThreadReady = false,
            RemoteCharacterState = RemoteCharacterStateNames.None,
            RemoteCharacterSpawnKey = 0,
            RemoteCharacterBase = 0,
            RemoteCharacterRecord = 0
        });
        Log("IPC disconnected");
    }

    private void OnProcessExited(object? sender, EventArgs value)
    {
        if (sender is Process process) _ = HandleProcessExitedAsync(process);
    }

    private async Task HandleProcessExitedAsync(Process exitedProcess)
    {
        await _lifecycleGate.WaitAsync().ConfigureAwait(false);
        try
        {
            if (!ReferenceEquals(_process, exitedProcess)) return;
            int? exitCode = null;
            try { exitCode = exitedProcess.ExitCode; }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
            await CloseSessionAsync().ConfigureAwait(false);
            DetachProcess();
            ClearProcessState();
            if (exitCode.HasValue)
            {
                var raw = unchecked((uint)exitCode.Value);
                Log($"AI.exe exited; exitCode={exitCode.Value} (0x{raw:X8})");
            }
            else
            {
                Log("AI.exe exited; exitCode unavailable");
            }
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    private async Task HandleInjectionFailureAsync(Exception exception)
    {
        Update(value => value with
        {
            IpcConnected = false,
            LastNativeException = exception.Message
        });
        Log($"Injection failed: {exception.Message}");
        await CloseSessionAsync().ConfigureAwait(false);
    }

    private bool HasTrackedProcess()
    {
        try { return _process is { HasExited: false }; }
        catch (InvalidOperationException) { return false; }
    }

    private void UpdateProcessPresence(AiProcessTarget target)
    {
        Update(value => value with
        {
            AiRunning = true,
            ProcessId = target.ProcessId,
            ExecutablePath = target.ExecutablePath,
            ModuleBase = target.ModuleBase
        });
    }

    private void MarkProcessAbsent()
    {
        if (_ipc is { IsReady: true }) return;
        ClearProcessState();
    }

    private void ClearProcessState()
    {
        var previous = Snapshot;
        Update(_ => NativeRuntimeSnapshot.Empty with
        {
            // Keep the last PID/version/log path after AI.exe exits so a
            // post-mortem diagnostics export can still locate native.log. A
            // subsequent ResetForNewProcess replaces the PID with the new game.
            ProcessId = previous.ProcessId,
            NativeDllVersion = previous.NativeDllVersion,
            NativeLogPath = previous.NativeLogPath,
            DllPath = previous.DllPath,
            LastCommand = previous.LastCommand,
            LastCommandResult = previous.LastCommandResult,
            LastNativeException = previous.LastNativeException
        });
    }

    private async Task CloseSessionAsync()
    {
        var session = Interlocked.Exchange(ref _ipc, null);
        if (session is null) return;
        UnsubscribeSession(session);
        await session.DisposeAsync().ConfigureAwait(false);
        Update(value => value with { IpcConnected = false });
    }

    private void DetachProcess()
    {
        var process = Interlocked.Exchange(ref _process, null);
        if (process is null) return;
        process.Exited -= OnProcessExited;
        process.Dispose();
    }

    private void Update(Func<NativeRuntimeSnapshot, NativeRuntimeSnapshot> transition)
    {
        NativeRuntimeSnapshot next;
        lock (_stateGate)
        {
            next = transition(_snapshot);
            if (next == _snapshot) return;
            _snapshot = next;
        }

        StateChanged?.Invoke(this, next);
    }

    private void Log(string message, string category = "Launcher") =>
        _diagnostics.Log(category, message);

    internal async Task ShutdownAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        await _lifecycleGate.WaitAsync().ConfigureAwait(false);
        try
        {
            await CloseSessionAsync().ConfigureAwait(false);
            DetachProcess();
            Log("Launcher shutdown completed.");
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public void Dispose()
    {
        ShutdownAsync().GetAwaiter().GetResult();
        _lifecycleGate.Dispose();
    }
}
