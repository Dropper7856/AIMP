using AIMP.Protocol;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Cryptography;

namespace AIMP.Launcher;

internal sealed class DllIpcSession : IAsyncDisposable
{
    private const int MaximumPendingRequests = 32;
    private readonly int _targetProcessId;
    private readonly NamedPipeServerStream _pipe;
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly SemaphoreSlim _pendingSlots = new(MaximumPendingRequests);
    private readonly ConcurrentDictionary<Guid, PendingIpcResponse> _pending = new();
    private readonly CancellationTokenSource _shutdown = new();
    private Task? _readLoop;
    private Task? _heartbeatLoop;
    private Guid _runtimeIdentity;
    // Step 5: one process-wide IPC sequence space, started from a cryptographic
    // random seed. IDs are never reset by a pipe reconnect or WorldGeneration
    // change; the random start also makes collision with a delayed result from a
    // previous Launcher process practically negligible if AI.exe remains alive.
    private static long s_processSequence = CreateProcessSequenceSeed();
    private int _disposed;

    internal DllIpcSession(int targetProcessId)
    {
        if (targetProcessId <= 0) throw new ArgumentOutOfRangeException(nameof(targetProcessId));
        _targetProcessId = targetProcessId;
        _pipe = new NamedPipeServerStream(
            ProtocolConstants.PipeName(targetProcessId),
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.WriteThrough | PipeOptions.CurrentUserOnly,
            64 * 1024,
            64 * 1024);
    }

    internal bool IsReady { get; private set; }
    internal Guid ConnectionIdentity { get; } = Guid.NewGuid();
    internal int TargetProcessId => _targetProcessId;

    internal bool AISyncAvailable { get; private set; }

    internal CapabilityStatePayload? LastCapabilityState { get; private set; }

    internal event EventHandler<CapabilityStatePayload>? CapabilityChanged;

    internal event EventHandler<CaptureStatePayload>? CaptureChanged;

    internal event EventHandler<LevelStatePayload>? LevelStateChanged;

    internal event EventHandler<ExperimentalErrorPayload>? NativeError;

    internal event EventHandler<GameCommandResultPayload>? GameCommandResultReceived;

    internal event EventHandler<WorldIntent>? WorldIntentReceived;

    internal event EventHandler<WorldMutationResult>? WorldMutationResultReceived;

    internal event EventHandler<NativePlayerEquipmentStatePayload>? PlayerSyncStateChanged;

    internal event EventHandler<NativePlayerActionEventPayload>? PlayerSyncActionProduced;

    internal event EventHandler<NativePlayerLifecycleStatePayload>? PlayerLifecycleStateChanged;
    internal event EventHandler<NativeCharacterIncarnationState>? CharacterIncarnationChanged;
    internal event EventHandler<NativePlayerHidingStatePayload>? PlayerHidingStateChanged;
    internal event EventHandler<NativePlayerAnimationStatePayload>? PlayerAnimationStateChanged;

    internal event EventHandler<NativeMissionSignalPayload>? MissionSignalProduced;

    internal event EventHandler<NativeItemSyncPickupPayload>? ItemSyncPickupProduced;

    internal event EventHandler<NativeItemSyncSpawnPayload>? ItemSyncSpawnProduced;
    internal event EventHandler<ReadOnlyMemory<byte>>? ThrowableProduced;


    internal event EventHandler<AlienPvPNativeFrame>? AlienPvPNativeProduced;
    internal event EventHandler<ReadOnlyMemory<byte>>? AISyncImpactProduced;
    internal event EventHandler<ReadOnlyMemory<byte>>? AISyncTransactionObservationProduced;
    internal event EventHandler<(MessageType Type, ReadOnlyMemory<byte> Bytes)>? AISyncExecutionProduced;

    internal event EventHandler<string>? Log;

    internal event EventHandler? ConnectionClosed;

    internal async Task<RuntimeHandshakeResult> AcceptAndHandshakeAsync(
        string executablePath,
        Guid runtimeIdentity,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        EnsureHandshakeArguments(executablePath, runtimeIdentity, timeout);
        using var scope = CreateTimeout(timeout, cancellationToken);
        await _pipe.WaitForConnectionAsync(scope.Token).ConfigureAwait(false);
        ValidateClientProcess();
        var hello = await ReadHelloAsync(scope.Token).ConfigureAwait(false);
        await SendHelloAckAsync(hello, scope.Token).ConfigureAwait(false);
        var build = await ReadBuildAsync(scope.Token).ConfigureAwait(false);
        await ValidateBuildAsync(executablePath, build, scope.Token).ConfigureAwait(false);
        await SendSessionConfigAsync(runtimeIdentity, scope.Token).ConfigureAwait(false);
        var ready = await ReadReadyAsync(scope.Token).ConfigureAwait(false);
        CompleteHandshake(runtimeIdentity, hello.DllVersion);
        return new RuntimeHandshakeResult(build, ready, hello.DllVersion);
    }

    internal async Task<GameCommandResultPayload> ExecuteGameCommandAsync(
        GameCommandExecutePayload request,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ValidateCommandRequest(request);
        var frame = await RequestAsync(MessageType.GameCommandExecute,
            MessageType.GameCommandResult, request.RequestId,
            JsonPayload.Serialize(request), timeout, cancellationToken).ConfigureAwait(false);
        var result = JsonPayload.Deserialize<GameCommandResultPayload>(frame.Payload.Span);
        ModelValidation.Validate(result);
        ValidateCorrelation(request.RequestId, request.LevelGeneration,
            result.RequestId, result.LevelGeneration, result.RuntimeIdentity);
        return result;
    }

    internal async Task<CapabilityStatePayload> QueryCapabilitiesAsync(
        CapabilityQueryPayload request,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        EnsureReady();
        ModelValidation.Validate(request);
        EnsureRuntimeIdentity(request.RuntimeIdentity);
        var frame = await RequestAsync(MessageType.CapabilityQuery,
            MessageType.CapabilityState, request.RequestId,
            JsonPayload.Serialize(request), timeout, cancellationToken).ConfigureAwait(false);
        var result = JsonPayload.Deserialize<CapabilityStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(result);
        ValidateCorrelation(request.RequestId, request.LevelGeneration,
            result.RequestId, result.LevelGeneration, result.RuntimeIdentity);
        return result;
    }

    internal async Task<DiagnosticSnapshotResultPayload> RequestDiagnosticsAsync(
        DiagnosticSnapshotRequestPayload request,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        ValidateDiagnosticRequest(request);
        var frame = await RequestAsync(MessageType.DiagnosticSnapshotRequest,
            MessageType.DiagnosticSnapshotResult, request.RequestId,
            JsonPayload.Serialize(request), timeout, cancellationToken).ConfigureAwait(false);
        var result = JsonPayload.Deserialize<DiagnosticSnapshotResultPayload>(frame.Payload.Span);
        ModelValidation.Validate(result);
        ValidateCorrelation(request.RequestId, request.LevelGeneration,
            result.RequestId, result.LevelGeneration, result.RuntimeIdentity);
        return result;
    }

    private static void EnsureHandshakeArguments(
        string executablePath,
        Guid runtimeIdentity,
        TimeSpan timeout)
    {
        if (!File.Exists(executablePath)) throw new FileNotFoundException("AI.exe was not found.");
        if (runtimeIdentity == Guid.Empty) throw new ArgumentException("Runtime identity is empty.");
        if (timeout <= TimeSpan.Zero || timeout > TimeSpan.FromMinutes(1))
            throw new ArgumentOutOfRangeException(nameof(timeout));
    }

    private CancellationTokenSource CreateTimeout(
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var source = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _shutdown.Token);
        source.CancelAfter(timeout);
        return source;
    }

    private void ValidateClientProcess()
    {
        var actual = PipeNativeMethods.GetClientProcessId(_pipe.SafePipeHandle);
        if (actual != _targetProcessId)
            throw new ProtocolException($"Pipe client PID {actual} does not match {_targetProcessId}.");
        Log?.Invoke(this, $"Native DLL connected to {ProtocolConstants.PipeName(actual)}.");
    }

    private async Task<HelloPayload> ReadHelloAsync(CancellationToken token)
    {
        var frame = await ReadExpectedAsync(MessageType.Hello, token).ConfigureAwait(false);
        var hello = JsonPayload.Deserialize<HelloPayload>(frame.Payload.Span);
        ValidateHello(hello);
        return hello;
    }

    private void ValidateHello(HelloPayload hello)
    {
        if (!IsValidHello(hello)) throw new ProtocolException("The native HELLO is invalid.");
        if (!string.Equals(hello.DllVersion, ProductInfo.ExpectedDllVersion,
                StringComparison.Ordinal))
            throw new ProtocolException(
                $"Native DLL {hello.DllVersion} is already loaded; restart AI.exe to load {ProductInfo.ExpectedDllVersion}.");
        if (!hello.UiHeapSynchronized)
            throw new ProtocolException("Native UI heap synchronization is unavailable; inspect ui_heap_guard. UI bridge payloads remain blocked.");
        AISyncAvailable = hello.AiSyncV1Available;
        if (!AISyncAvailable)
            Log?.Invoke(this, "AISync unavailable: native hooks were not installed; inspect aisync_v2_install. IPC remains connected for PlayerSync, WorldSync and diagnostics.");
    }

    private bool IsValidHello(HelloPayload hello) =>
        hello.ProtocolVersion == ProtocolConstants.Version
        && hello.ProcessId == _targetProcessId
        && hello.ObservationOnly
        && hello.Nonce is { Length: 32 }
        && hello.Nonce.All(Uri.IsHexDigit);

    private Task SendHelloAckAsync(HelloPayload hello, CancellationToken token)
    {
        var payload = new HelloAckPayload(ProtocolConstants.Version,
            ProductInfo.LauncherVersion, _targetProcessId, hello.Nonce);
        return SendAsync(MessageType.HelloAck, JsonPayload.Serialize(payload), token);
    }

    private async Task<BuildInfoPayload> ReadBuildAsync(CancellationToken token)
    {
        var frame = await ReadExpectedAsync(MessageType.BuildInfo, token).ConfigureAwait(false);
        var value = JsonPayload.Deserialize<BuildInfoPayload>(frame.Payload.Span);
        if (value.AiSha256 is not { Length: 64 } || !value.AiSha256.All(Uri.IsHexDigit))
            throw new ProtocolException("The native build fingerprint is invalid.");
        return value;
    }

    private static async Task ValidateBuildAsync(
        string executablePath,
        BuildInfoPayload build,
        CancellationToken token)
    {
        await using var stream = File.OpenRead(executablePath);
        var digest = await SHA256.HashDataAsync(stream, token).ConfigureAwait(false);
        var localHash = Convert.ToHexString(digest);
        if (!string.Equals(localHash, build.AiSha256, StringComparison.OrdinalIgnoreCase))
            throw new ProtocolException("The native and launcher AI.exe fingerprints differ.");
    }

    private Task SendSessionConfigAsync(Guid identity, CancellationToken token)
    {
        var config = CreateSessionConfig(identity);
        ModelValidation.Validate(config);
        return SendAsync(MessageType.SessionConfig, JsonPayload.Serialize(config), token);
    }

    private static SessionConfigPayload CreateSessionConfig(Guid identity) => new(
        // IPC bootstrap identity only. The real multiplayer PeerId/Host role is
        // bound later through WorldSyncLifecycle once a lobby actually exists.
        Guid.NewGuid(),
        1,
        identity,
        "LauncherBootstrap",
        false,
        "127.0.0.1",
        ProtocolConstants.DefaultGamePort,
        0,
        false);

    private async Task<NetZReadyPayload> ReadReadyAsync(CancellationToken token)
    {
        var frame = await ReadExpectedAsync(MessageType.NetZReady, token).ConfigureAwait(false);
        var ready = JsonPayload.Deserialize<NetZReadyPayload>(frame.Payload.Span);
        if (!IsValidReady(ready))
            throw new ProtocolException("The native runtime reported an unsafe ready state.");
        return ready;
    }

    private static bool IsValidReady(NetZReadyPayload value) =>
        string.Equals(value.Mode, "observation", StringComparison.Ordinal)
        && !value.NativeReplicationReady
        && value.BlockedGates is not null
        && value.BlockedGates.Count <= 64
        && value.BlockedGates.All(gate => !string.IsNullOrWhiteSpace(gate) && gate.Length <= 128);

    private async Task<AimpFrame> ReadExpectedAsync(
        MessageType expected,
        CancellationToken token)
    {
        var frame = await FrameCodec.ReadAsync(_pipe, token).ConfigureAwait(false);
        if (frame.Type == MessageType.Error) ThrowRemoteError(frame);
        if (frame.Type != expected)
            throw new ProtocolException($"Expected {expected}, received {frame.Type}.");
        return frame;
    }

    private static void ThrowRemoteError(AimpFrame frame)
    {
        var error = JsonPayload.Deserialize<ErrorPayload>(frame.Payload.Span);
        throw new ProtocolException($"Native IPC error ({error.Code}).");
    }

    private void CompleteHandshake(Guid runtimeIdentity, string dllVersion)
    {
        _runtimeIdentity = runtimeIdentity;
        IsReady = true;
        Log?.Invoke(this, $"IPC handshake completed with native DLL {dllVersion}.");
        _readLoop = ReadLoopAsync(_shutdown.Token);
        _heartbeatLoop = HeartbeatLoopAsync(_shutdown.Token);
    }

    private void ValidateCommandRequest(GameCommandExecutePayload request)
    {
        EnsureReady();
        ModelValidation.Validate(request);
        if (request.RuntimeIdentity != _runtimeIdentity)
            throw new ProtocolException("The command runtime identity does not match this session.");
    }

    private void ValidateDiagnosticRequest(DiagnosticSnapshotRequestPayload request)
    {
        EnsureReady();
        ModelValidation.Validate(request);
        if (request.RuntimeIdentity != _runtimeIdentity)
            throw new ProtocolException("The diagnostics runtime identity does not match this session.");
    }

    private void EnsureReady()
    {
        if (!IsReady || _runtimeIdentity == Guid.Empty)
            throw new InvalidOperationException("The native IPC connection is not ready.");
    }

    private async Task<AimpFrame> RequestAsync(
        MessageType requestType,
        MessageType responseType,
        Guid requestId,
        ReadOnlyMemory<byte> payload,
        TimeSpan timeout,
        CancellationToken token)
    {
        await _pendingSlots.WaitAsync(token).ConfigureAwait(false);
        var pending = CreatePending(responseType);
        if (!_pending.TryAdd(requestId, pending)) ThrowDuplicateRequest();
        try
        {
            await SendAsync(requestType, payload, token).ConfigureAwait(false);
            return await pending.Completion.Task.WaitAsync(timeout, token).ConfigureAwait(false);
        }
        finally
        {
            ReleasePending(requestId);
        }
    }

    private static PendingIpcResponse CreatePending(MessageType responseType) => new(
        responseType,
        new TaskCompletionSource<AimpFrame>(TaskCreationOptions.RunContinuationsAsynchronously));

    private void ThrowDuplicateRequest()
    {
        _pendingSlots.Release();
        throw new InvalidOperationException("The request ID is already pending.");
    }

    private void ReleasePending(Guid requestId)
    {
        if (_pending.TryRemove(requestId, out _)) _pendingSlots.Release();
    }

    private void ValidateCorrelation(
        Guid expectedId,
        int expectedGeneration,
        Guid actualId,
        int actualGeneration,
        Guid actualIdentity)
    {
        var generationMatches = expectedGeneration == 0
            ? actualGeneration >= 0
            : expectedGeneration == actualGeneration;
        if (expectedId != actualId || !generationMatches)
            throw new ProtocolException("The native response correlation is invalid.");
        if (actualIdentity != _runtimeIdentity)
            throw new ProtocolException("The native response runtime identity is invalid.");
    }

    private void EnsureRuntimeIdentity(Guid value)
    {
        if (value != _runtimeIdentity)
            throw new ProtocolException("The native runtime identity is invalid.");
    }

    private async Task ReadLoopAsync(CancellationToken token)
    {
        try
        {
            await ReadFramesAsync(token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception exception) when (IsConnectionFailure(exception))
        {
            Log?.Invoke(this, $"IPC connection stopped: {exception.Message}");
            FailPending(exception);
        }
        finally
        {
            CloseConnection();
        }
    }

    private async Task ReadFramesAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            var frame = await FrameCodec.ReadAsync(_pipe, token).ConfigureAwait(false);
            DispatchFrame(frame);
        }
    }

    private void DispatchFrame(AimpFrame frame)
    {
        if (TryHandleStateFrame(frame)) return;
        if (TryHandleResponseFrame(frame)) return;
        if (frame.Type == MessageType.WorldIntent)
        {
            try
            {
                var intent = WorldReplicationCodec.DecodeIntent(frame.Payload.Span);
                WorldIntentReceived?.Invoke(this, intent);
            }
            catch (ProtocolException exception)
            {
                // A malformed/unsupported local WorldIntent must be isolated to that
                // event. Closing the DLL pipe also stops Character replication and
                // prevents subsequent diagnostics, which is disproportionate to a
                // single rejected WorldSync observation.
                Log?.Invoke(this,
                    $"Rejected invalid WORLD_INTENT without closing IPC: {exception.Message}");
            }
        }
        else if (frame.Type == MessageType.WorldMutationResult)
        {
            if (frame.Sequence == 0)
            {
                Log?.Invoke(this, "Rejected WORLD_MUTATION_RESULT with zero NativeDispatchId.");
                return;
            }
            var result = WorldReplicationCodec.DecodeResult(frame.Payload.Span) with
            {
                NativeDispatchId = frame.Sequence
            };
            WorldMutationResultReceived?.Invoke(this, result);
        }
        else if (frame.Type == MessageType.Heartbeat) ValidateEmpty(frame, "HEARTBEAT");
        else if (frame.Type == MessageType.Shutdown) ValidateEmpty(frame, "SHUTDOWN");
        else if (frame.Type == MessageType.Error) ThrowRemoteError(frame);
        else throw new ProtocolException($"Unexpected native message: {frame.Type}.");
    }

    private bool TryHandleStateFrame(AimpFrame frame)
    {
        switch (frame.Type)
        {
            case MessageType.AlienPvPNative:
                if(!IsReady)throw new ProtocolException("PvP data preceded the native handshake.");
                var alien=AlienPvPNativeCodec.Decode(frame.Payload.Span);
                if(!AlienPvPDelivery.NativeToManaged(alien.Kind))
                    throw new ProtocolException("Native tried to grant its own PvP authority.");
                AlienPvPNativeProduced?.Invoke(this,alien);
                return true;
            case MessageType.AISyncExecutionBinding:
            case MessageType.AISyncExecutionReceipt:
            case MessageType.AISyncHostBatch:
            case MessageType.AISyncHostState:
            case MessageType.AISyncHostRoute:
            case MessageType.AISyncHostControl:
                HandleAISyncExecution(frame);
                return true;
            case MessageType.CapabilityState: HandleCapability(frame); return true;
            case MessageType.CaptureState: HandleCapture(frame); return true;
            case MessageType.LevelState: HandleLevel(frame); return true;
            case MessageType.PlayerSyncState: HandlePlayerSyncState(frame); return true;
            case MessageType.PlayerSyncAction: HandlePlayerSyncAction(frame); return true;
            case MessageType.PlayerLifecycleState: HandlePlayerLifecycleState(frame); return true;
            case MessageType.CharacterIncarnationState:
                HandleCharacterIncarnation(frame);
                return true;
            case MessageType.PlayerHidingState:
                if (!IsReady) throw new ProtocolException("Hiding observation arrived before the DLL handshake.");
                var hiding = JsonPayload.Deserialize<NativePlayerHidingStatePayload>(frame.Payload.Span);
                if (hiding.SchemaVersion != 1 || hiding.NativeGeneration == 0 || hiding.Character < 0x10000
                    || hiding.Serial == 0 || (hiding.Presentation != "0"
                        && !PlayerHidingPresentationCodec.TryDecode(hiding.Presentation, out _, out _)))
                    throw new ProtocolException("Invalid native hiding observation.");
                PlayerHidingStateChanged?.Invoke(this, hiding);
                return true;
            case MessageType.PlayerAnimationState:
                if (!IsReady) throw new ProtocolException("Animation observation arrived before the DLL handshake.");
                var animation = JsonPayload.Deserialize<NativePlayerAnimationStatePayload>(frame.Payload.Span);
                if (animation.SchemaVersion != 1 || animation.NativeGeneration == 0 || animation.Character < 0x10000
                    || animation.Serial == 0 || !PlayerAnimationCodec.TryDecode(animation.Animation, out _))
                    throw new ProtocolException("Invalid native animation observation.");
                PlayerAnimationStateChanged?.Invoke(this, animation);
                return true;
            case MessageType.MissionSignalProduced: HandleMissionSignal(frame); return true;
            case MessageType.ItemSyncPickup: HandleItemSyncPickup(frame); return true;
            case MessageType.ItemSyncSpawn: HandleItemSyncSpawn(frame); return true;
            case MessageType.ThrowableEvent:
                ThrowableSyncCodec.Validate(frame.Payload.Span);
                ThrowableProduced?.Invoke(this, frame.Payload);
                return true;
            case MessageType.AISyncState:
                Log?.Invoke(this, "Retired AISync state message ignored; execution stream required.");
                return true;
            case MessageType.AISyncImpact:
                HandleAISyncImpact(frame);
                return true;
            case MessageType.AISyncTransactionObservation:
                HandleAISyncTransactionObservation(frame);
                return true;
            default: return false;
        }
    }

    private void HandleCharacterIncarnation(AimpFrame frame)
    {
        if (!IsReady) throw new ProtocolException("Character lifetime arrived before the DLL handshake.");
        NativeCharacterIncarnationState state;
        try { state = NativeCharacterIncarnationCodec.Decode(frame.Payload.Span); }
        catch (ProtocolException exception)
        {
            state = new(0,0,0,0);
            Log?.Invoke(this, $"Character lifetime unavailable after malformed native observation: {exception.Message}");
        }
        CharacterIncarnationChanged?.Invoke(this, state);
    }
    private void HandleAISyncTransactionObservation(AimpFrame frame)
    {
        try { AISyncShadowCodec.ValidateNativeObservation(frame.Payload.Span); }
        catch (ProtocolException exception)
        {
            Log?.Invoke(this, $"Rejected optional AISync shadow observation without closing IPC: {exception.Message}");
            return;
        }
        AISyncTransactionObservationProduced?.Invoke(this, frame.Payload);
    }
    private void HandleAISyncExecution(AimpFrame frame)
    {
        try { ValidateAISyncExecution(frame); }
        catch (ProtocolException exception) { Log?.Invoke(this, $"Rejected AISync execution disposition: {exception.Message}"); return; }
        AISyncExecutionProduced?.Invoke(this, (frame.Type, frame.Payload));
    }
    private static void ValidateAISyncExecution(AimpFrame frame)
    {
        if (frame.Type is MessageType.AISyncExecutionBinding or MessageType.AISyncHostControl)
        {
            var control = AISyncExecutionControlCodec.Decode(frame.Payload.Span);
            if (control.Kind is not (AISyncExecutionControlKind.Ready or AISyncExecutionControlKind.Recovery or AISyncExecutionControlKind.CheckpointAck))
                throw new ProtocolException("Invalid native execution control direction.");
        }
        else if (frame.Type == MessageType.AISyncHostState)
            _ = AISyncCausalStateCodec.Decode(frame.Payload.Span);
        else if (frame.Type == MessageType.AISyncHostRoute)
            _ = AISyncRouteCodec.Decode(frame.Payload.Span, AISyncRouteCodec.DecodeKey(frame.Payload.Span));
        else if (frame.Type == MessageType.AISyncHostBatch)
            _ = AISyncExecutionCodec.DecodeBatch(frame.Payload.Span);
        else _ = AISyncExecutionCodec.DecodeReceipt(frame.Payload.Span);
    }

    private void HandleAISyncImpact(AimpFrame frame)
    {
        try { AISyncCodec.ValidateNativeImpact(frame.Payload.Span); }
        catch (ProtocolException exception)
        {
            Log?.Invoke(this, $"Rejected AISYNC_IMPACT sequence={frame.Sequence} bytes={frame.Payload.Length} without closing IPC: {exception.Message}");
            return;
        }
        AISyncImpactProduced?.Invoke(this, frame.Payload);
    }

    private bool TryHandleResponseFrame(AimpFrame frame)
    {
        switch (frame.Type)
        {
            case MessageType.GameCommandResult: CompleteGameCommand(frame); return true;
            case MessageType.DiagnosticSnapshotResult: CompleteDiagnostics(frame); return true;
            case MessageType.ExperimentalError: CompleteNativeError(frame); return true;
            default: return false;
        }
    }

    private void HandleCapability(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<CapabilityStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        EnsureRuntimeIdentity(value.RuntimeIdentity);
        LastCapabilityState = value;
        CompletePending(value.RequestId, frame);
        CapabilityChanged?.Invoke(this, value);
    }

    private void HandleCapture(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<CaptureStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        EnsureRuntimeIdentity(value.RuntimeIdentity);
        CaptureChanged?.Invoke(this, value);
    }

    private void HandleLevel(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<LevelStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        EnsureRuntimeIdentity(value.RuntimeIdentity);
        LevelStateChanged?.Invoke(this, value);
    }

    private void HandlePlayerSyncState(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativePlayerEquipmentStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        PlayerSyncStateChanged?.Invoke(this, value);
    }

    private void HandlePlayerSyncAction(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativePlayerActionEventPayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        PlayerSyncActionProduced?.Invoke(this, value);
    }

    private void HandlePlayerLifecycleState(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativePlayerLifecycleStatePayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        PlayerLifecycleStateChanged?.Invoke(this, value);
    }

    private void HandleMissionSignal(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativeMissionSignalPayload>(frame.Payload.Span);
        if (value.LevelGeneration <= 0 || value.Sequence == 0 || value.Signal == 0
            // 3 = public input, 4 = scheduled transition (native MissionSync::Capture).
            || value.InterfaceSelector is not (1 or 2 or 3 or 4) || !ModelValidation.IsWorldEntityIdText(value.EntityId))
            throw new ProtocolException("Invalid native mission signal.");
        MissionSignalProduced?.Invoke(this, value);
    }

    private void HandleItemSyncPickup(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativeItemSyncPickupPayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        ItemSyncPickupProduced?.Invoke(this, value);
    }

    private void HandleItemSyncSpawn(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<NativeItemSyncSpawnPayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        ItemSyncSpawnProduced?.Invoke(this, value);
    }

    private void CompleteGameCommand(AimpFrame frame)
    {
        GameCommandResultPayload? value = null;
        try
        {
            value = JsonPayload.Deserialize<GameCommandResultPayload>(frame.Payload.Span);
            ModelValidation.Validate(value);
            EnsureRuntimeIdentity(value.RuntimeIdentity);
        }
        catch (ProtocolException exception)
        {
            // A malformed diagnostic/command result must fail only that command. Older
            // behavior let this ProtocolException escape the read loop, which closed an
            // otherwise healthy DLL IPC session and forced AI.exe to be restarted.
            var failed = false;
            if (value is not null && value.RequestId != Guid.Empty &&
                _pending.TryGetValue(value.RequestId, out var correlated) &&
                correlated.ExpectedType == MessageType.GameCommandResult)
            {
                failed = correlated.Completion.TrySetException(exception);
            }

            if (!failed)
            {
                foreach (var pending in _pending.Values)
                {
                    if (pending.ExpectedType == MessageType.GameCommandResult)
                        pending.Completion.TrySetException(exception);
                }
            }

            Log?.Invoke(this,
                $"Rejected invalid GAME_COMMAND_RESULT without closing IPC: {exception.Message}");
            return;
        }

        if (value is null) return;

        // A queued acknowledgement owns the pending request; a later final result is an event.
        if (!CompletePending(value.RequestId, frame))
            GameCommandResultReceived?.Invoke(this, value);
    }

    private void CompleteDiagnostics(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<DiagnosticSnapshotResultPayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        EnsureRuntimeIdentity(value.RuntimeIdentity);
        CompletePending(value.RequestId, frame);
    }

    private void CompleteNativeError(AimpFrame frame)
    {
        var value = JsonPayload.Deserialize<ExperimentalErrorPayload>(frame.Payload.Span);
        ModelValidation.Validate(value);
        EnsureRuntimeIdentity(value.RuntimeIdentity);
        if (_pending.TryGetValue(value.RequestId, out var pending))
            pending.Completion.TrySetException(new ProtocolException($"Native error ({value.Code})."));
        NativeError?.Invoke(this, value);
    }

    private bool CompletePending(Guid requestId, AimpFrame frame)
    {
        if (!_pending.TryGetValue(requestId, out var pending)) return false;
        if (pending.ExpectedType == frame.Type) return pending.Completion.TrySetResult(frame);
        return pending.Completion.TrySetException(new ProtocolException(
            $"Expected {pending.ExpectedType}, received {frame.Type}."));
    }

    private static void ValidateEmpty(AimpFrame frame, string name)
    {
        if (!frame.Payload.IsEmpty) throw new ProtocolException($"{name} must have no payload.");
    }

    private static bool IsConnectionFailure(Exception exception) =>
        exception is IOException or ObjectDisposedException or ProtocolException;

    private void FailPending(Exception exception)
    {
        foreach (var pending in _pending.Values)
            pending.Completion.TrySetException(exception);
    }

    private void CloseConnection()
    {
        var wasReady = IsReady;
        IsReady = false;
        if (wasReady) ConnectionClosed?.Invoke(this, EventArgs.Empty);
    }

    private async Task HeartbeatLoopAsync(CancellationToken token)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
        try
        {
            while (await timer.WaitForNextTickAsync(token).ConfigureAwait(false))
                await SendAsync(MessageType.Heartbeat, ReadOnlyMemory<byte>.Empty, token)
                    .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception exception) when (IsConnectionFailure(exception))
        {
            Log?.Invoke(this, $"IPC heartbeat stopped: {exception.Message}");
        }
    }

    internal ulong ReserveWorldMutationDispatchId()
    {
        EnsureReady();
        return NextProcessSequence();
    }

    internal Task SendAlienPvPAsync(AlienPvPNativeFrame frame,CancellationToken token)
    {
        EnsureReady();
        if(!AlienPvPDelivery.ManagedToNative(frame.Kind))
            throw new ProtocolException("Launcher cannot fabricate native readiness or input observations.");
        return SendAsync(MessageType.AlienPvPCommand,AlienPvPNativeCodec.Encode(frame),token);
    }

    internal Task SendAISyncAsync(MessageType type, ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        EnsureReady();
        if (type is not (MessageType.AISyncConfigure or MessageType.AISyncImpact))
            throw new ArgumentOutOfRangeException(nameof(type));
        return SendAsync(type, payload, token);
    }
    internal Task SendAISyncExecutionAsync(MessageType type, ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        if (type is not (MessageType.AISyncExecutionBinding or MessageType.AISyncExecutionBatch or MessageType.AISyncExecutionRetry or MessageType.AISyncHostBinding or MessageType.AISyncClientRoute or MessageType.AISyncClientState))
            throw new ArgumentOutOfRangeException(nameof(type));
        if (type is MessageType.AISyncExecutionBinding or MessageType.AISyncHostBinding) {
            if (AISyncExecutionControlCodec.Decode(payload.Span).Kind == AISyncExecutionControlKind.Ready)
                throw new ProtocolException("Launcher cannot forge native execution readiness.");
        }
        else if (type == MessageType.AISyncClientState)
            _ = AISyncCausalStateCodec.Decode(payload.Span);
        else if (type == MessageType.AISyncClientRoute)
            _ = AISyncRouteCodec.Decode(payload.Span, AISyncRouteCodec.DecodeKey(payload.Span));
        else _ = AISyncExecutionCodec.DecodeBatch(payload.Span);
        return SendAsync(type, payload, token);
    }

    internal Task SendThrowableAsync(MessageType type, ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        EnsureReady();
        if (type is not (MessageType.ThrowableConfigure or MessageType.ThrowableEvent))
            throw new ArgumentOutOfRangeException(nameof(type));
        return SendAsync(type, payload, token);
    }

    internal async Task<bool> SendVoiceAsync(MessageType type, ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        EnsureReady();
        if (type is not (MessageType.VoiceConfigure or MessageType.VoicePcmFrame))
            throw new ArgumentOutOfRangeException(nameof(type));
        // Bounded: AISync/position frames hold the pipe briefly; never queue audio behind a stall.
        if (!await _writeLock.WaitAsync(TimeSpan.FromMilliseconds(15), token).ConfigureAwait(false)) return false;
        try
        {
            // Once a shared-pipe frame starts, only pipe shutdown may cancel it.
            // A voice timeout must never leave half a frame ahead of gameplay.
            await FrameCodec.WriteAsync(_pipe, new AimpFrame(type, 0, NextProcessSequence(), payload),
                _shutdown.Token).ConfigureAwait(false);
            return true;
        }
        finally { _writeLock.Release(); }
    }

    internal Task SendWorldSyncLifecycleAsync(
        WorldSyncLifecyclePayload lifecycle,
        CancellationToken cancellationToken = default)
    {
        EnsureReady();
        ModelValidation.Validate(lifecycle);
        return SendAsync(
            MessageType.WorldSyncLifecycle,
            JsonPayload.Serialize(lifecycle),
            cancellationToken);
    }

    internal Task SendWorldMutationAsync(
        WorldMutation mutation,
        ulong nativeDispatchId,
        CancellationToken cancellationToken = default)
    {
        EnsureReady();
        if (nativeDispatchId == 0)
            throw new ArgumentOutOfRangeException(nameof(nativeDispatchId));
        WorldReplicationValidation.Validate(mutation);
        return SendFrameAsync(
            MessageType.WorldMutation,
            WorldReplicationCodec.EncodeMutation(mutation),
            nativeDispatchId,
            cancellationToken);
    }

    internal Task SendWorldIntentRejectedAsync(
        WorldIntent intent,
        CancellationToken cancellationToken = default)
    {
        EnsureReady();
        WorldReplicationValidation.Validate(intent);
        return SendAsync(MessageType.WorldIntentRejected,
            WorldReplicationCodec.EncodeIntent(intent), cancellationToken);
    }

    internal Task SendWorldIntentAbandonedAsync(
        WorldIntent intent,
        CancellationToken cancellationToken = default)
    {
        EnsureReady();
        WorldReplicationValidation.Validate(intent);
        return SendAsync(MessageType.WorldIntentAbandoned,
            WorldReplicationCodec.EncodeIntent(intent), cancellationToken);
    }

    private static long CreateProcessSequenceSeed()
    {
        Span<byte> bytes = stackalloc byte[sizeof(long)];
        RandomNumberGenerator.Fill(bytes);
        return BitConverter.ToInt64(bytes);
    }

    private static ulong NextProcessSequence()
    {
        // The random process seed prevents a practical collision with delayed
        // results left in a still-running AI.exe after a Launcher process restart.
        // Within this Launcher process the value then advances monotonically modulo
        // 2^64. Zero remains reserved and is skipped on theoretical full wrap.
        while (true)
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref s_processSequence));
            if (sequence != 0) return sequence;
        }
    }

    private Task SendAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken token) =>
        SendFrameAsync(type, payload, NextProcessSequence(), token);

    private async Task SendFrameAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong sequence,
        CancellationToken token)
    {
        if (sequence == 0) throw new ArgumentOutOfRangeException(nameof(sequence));
        await _writeLock.WaitAsync(token).ConfigureAwait(false);
        try
        {
            var frame = new AimpFrame(type, 0, sequence, payload);
            await FrameCodec.WriteAsync(_pipe, frame, token).ConfigureAwait(false);
        }
        catch { _pipe.Dispose(); throw; } // A partial frame must never share the pipe with later traffic.
        finally
        {
            _writeLock.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        // Closing only this pipe keeps the injected runtime available for a later reconnect.
        _shutdown.Cancel();
        _pipe.Dispose();
        FailPending(new IOException("The native IPC session was closed."));
        await AwaitLoopAsync(_readLoop).ConfigureAwait(false);
        await AwaitLoopAsync(_heartbeatLoop).ConfigureAwait(false);
        IsReady = false;
        _shutdown.Dispose();
    }

    private static async Task AwaitLoopAsync(Task? task)
    {
        if (task is null) return;
        try
        {
            await task.ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (ObjectDisposedException) { }
    }
}
