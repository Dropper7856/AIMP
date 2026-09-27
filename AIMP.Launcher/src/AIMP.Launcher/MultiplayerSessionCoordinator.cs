using AIMP.Protocol;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace AIMP.Launcher;

internal sealed record MultiplayerSessionView(
    string NetworkState,
    bool IsHost,
    bool Connected,
    bool LocalGameplayReady,
    bool RemoteGameplayReady,
    string RemotePlayerName)
{
    internal static MultiplayerSessionView Disconnected { get; } = new(
        "Disconnected", false, false, false, false, string.Empty);
}

internal sealed class MultiplayerSessionCoordinator : IDisposable
{
    private readonly NativeRuntimeController _controller;
    private readonly LauncherSettingsStore _settingsStore;
    private readonly ScaleformBridgeListener _bridge;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly SemaphoreSlim _commandGate = new(1, 1);
    private readonly SemaphoreSlim _spawnGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private readonly object _stateGate = new();
    private readonly GameplaySpawnReadinessGate _gameplayReadiness = new();
    private LobbyHost? _host;
    private LobbyClient? _client;
    private LobbySnapshotPayload? _lobby;
    private LobbyStartPayload? _start;
    private LocalPlayerPosition _position;
    private bool _positionAvailable;
    private float _yaw;
    private PlayerMoveType _moveType = PlayerMoveType.Walk;
    private float _nativeMoveSpeed = RemoteMovementPlanner.LegacyWalkFallbackMagnitude;
    private LocalPlayerPosition _previousMovementPosition;
    private DateTimeOffset _previousMovementSampleAtUtc;
    private bool _hasPreviousMovementSample;
    private float _observedLocalPlanarSpeed;
    private bool _uiGameplayLoaded;
    private bool _localGameplayReady;
    private bool _localReadySending;
    private bool _remoteGameplayReady;
    private readonly Dictionary<ulong, uint> _remoteCharacters = new();
    private readonly Dictionary<ulong, RemotePositionTrack> _remotePositions = new();
    private readonly HashSet<ulong> _remoteGameplayReadyPeers = new();
    private readonly HashSet<Guid> _processedSpawnRequests = new();
    private SpawnPositionRequestPayload? _pendingSpawnPositionRequest;
    private bool _spawnPositionSending;
    private bool _spawnCommandCollecting;
    private SpawnRemotePlayersPayload? _pendingSpawnRequest;
    private bool _spawnRequestRunning;
    private bool _remoteSpawnConvergenceRunning;
    private readonly Dictionary<ulong, int> _remoteSpawnRetryAttempts = new();
    private readonly Dictionary<ulong, DateTimeOffset> _remoteSpawnRetryNotBeforeUtc = new();
    private static readonly TimeSpan RemoteSpawnPositionFreshness = TimeSpan.FromSeconds(1);
    private PlayerPositionSnapshotPayload? _pendingLocalPosition;
    private bool _positionSendRunning;
    private ulong _localPositionSequence;
    private NativePlayerEquipmentStatePayload? _localEquipmentState;
    private NativePlayerEquipmentStatePayload? _lastQueuedEquipmentState;
    private DateTimeOffset _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
    private PlayerEquipmentSnapshotPayload? _pendingLocalEquipment;
    private bool _equipmentSendRunning;
    private ulong _localEquipmentSequence;
    private readonly Dictionary<ulong, PlayerEquipmentSnapshotPayload> _remoteEquipment = new();
    private readonly SemaphoreSlim _remoteEquipmentProvisionGate = new(1, 1);
    private bool _movementCommandRunning;
    private string _lastMovementFailure = string.Empty;
    private DateTimeOffset _lastMovementFailureLoggedAt;
    private WorldNetworkContext? _localWorldContext;
    private int _worldSessionGeneration;
    private int _localWorldRuntimeGeneration;
    private Task<PreparedWorldManifest?>? _automaticWorldManifestPreparation;
    private Task<RemoteEquipmentCatalogSet?>? _remoteEquipmentCatalogPreparation;
    private bool _worldActivationRunning;
    private bool _worldActivationAttempted;
    private bool _worldSyncEnabledForSession = true;
    private TaskCompletionSource<WorldMutationResult>? _pendingWorldProbeResult;
    private TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>? _pendingWorldProbeAckResult;
    private HashSet<ulong>? _pendingWorldProbeExpectedPeers;
    private readonly Dictionary<ulong, WorldNetworkMutationAck> _pendingWorldProbeAcks = new();
    private ulong _pendingWorldProbeSequence;
    private DateTimeOffset _nextWorldMutationFaultSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldMutationFaults;
    private DateTimeOffset _nextWorldIntentRejectSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldIntentRejects;
    private string _lastWorldIntentRejectReason = string.Empty;
    private DateTimeOffset _nextWorldIntentAbandonSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldIntentAbandons;
    private string _lastWorldIntentAbandonReason = string.Empty;
    private readonly SemaphoreSlim _worldIntentGate = new(1, 1);
    private readonly SemaphoreSlim _worldMutationApplyGate = new(1, 1);
    private int _queuedWorldIntents;
    private enum WorldBusLifecycleState : byte
    {
        Inactive,
        AwaitingCompatibleWorldContext,
        Active,
        RecoveryRequired
    }
    private Guid _nativeAuthoritySessionId;
    private ulong _nativeAuthorityPeerId;
    private bool _nativeAuthorityIsHost;
    private int _nativeAuthorityGeneration;
    private bool _nativeAuthorityBound;
    private bool _nativeWorldBusActive;
    private readonly record struct AwaitAuthorityCause(ulong SourcePeer, ulong ObservationId);
    private sealed class ClientWorldTransaction(
        ulong startSequence,
        int rootCount,
        WorldNetworkContext worldContext,
        WorldAuthorityContextIdentity? hostAuthorityContext)
    {
        internal ulong StartSequence { get; } = startSequence;
        internal int RootCount { get; } = rootCount;
        internal WorldNetworkContext WorldContext { get; } = worldContext;
        internal WorldAuthorityContextIdentity? HostAuthorityContext { get; } = hostAuthorityContext;
        internal int ReceivedCount { get; set; }
        internal int ResultCount { get; set; }
        internal bool Failed { get; set; }
        internal HashSet<AwaitAuthorityCause> LocalAwaitCauses { get; } = new();
        internal TaskCompletionSource<bool> Completion { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        internal ulong EndSequence => StartSequence + checked((ulong)RootCount) - 1U;
    }
    private sealed record NativeWorldDispatch(
        ulong NativeDispatchId,
        ulong AuthoritySequence,
        ClientWorldTransaction Transaction);
    private ClientWorldTransaction? _activeWorldTransaction;
    // Step 5: correlation is exact and local to Launcher <-> DLL. Authority
    // sequence remains bookkeeping only and is never used to guess which native
    // dispatch completed.
    private readonly Dictionary<ulong, NativeWorldDispatch> _worldResultDispatches = new();
    private WorldAuthorityContextIdentity? _currentHostAuthorityContext;
    private WorldAuthorityContextIdentity? _worldRecoveryAuthorityContext;
    private readonly HashSet<AwaitAuthorityCause> _pendingAwaitAuthorityCauses = new();
    private static bool SameWorldContext(WorldNetworkContext left, WorldNetworkContext right) =>
        left.SessionId == right.SessionId
        && left.WorldGeneration == right.WorldGeneration
        && left.Manifest == right.Manifest
        && string.Equals(left.ManifestSha256, right.ManifestSha256,
            StringComparison.OrdinalIgnoreCase);

    private WorldBusLifecycleState GetWorldBusLifecycleStateLocked()
    {
        if (!_worldSyncEnabledForSession || !_nativeAuthorityBound
            || (_host is null && _client is null) || _start is null)
            return WorldBusLifecycleState.Inactive;
        if (_worldRecoveryRequired) return WorldBusLifecycleState.RecoveryRequired;
        if (_worldRecoveryRequestRunning || _worldRecoveryTargetSequence != 0)
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        if (!_nativeWorldBusActive || _localWorldContext is not { Ready: true })
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        if (_host is not null && !_host.AreWorldPeersCompatible())
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        return WorldBusLifecycleState.Active;
    }

    private async Task<bool> SetNativeWorldSyncLifecycleAsync(
        Guid sessionId,
        ulong peerId,
        bool isHost,
        int generation,
        bool authorityBound,
        bool worldBusActive,
        string reason)
    {
        if (sessionId == Guid.Empty || peerId == 0) return false;
        var payload = new WorldSyncLifecyclePayload(
            sessionId,
            peerId,
            isHost,
            Math.Max(0, generation),
            authorityBound,
            authorityBound && worldBusActive,
            reason);
        try
        {
            await _controller.SendWorldSyncLifecycleAsync(payload, _lifetime.Token)
                .ConfigureAwait(false);
            lock (_stateGate)
            {
                if (authorityBound)
                {
                    _nativeAuthoritySessionId = sessionId;
                    _nativeAuthorityPeerId = peerId;
                    _nativeAuthorityIsHost = isHost;
                    _nativeAuthorityGeneration = payload.LevelGeneration;
                    _nativeAuthorityBound = true;
                    _nativeWorldBusActive = payload.WorldBusActive;
                }
                else
                {
                    _nativeAuthoritySessionId = Guid.Empty;
                    _nativeAuthorityPeerId = 0;
                    _nativeAuthorityIsHost = false;
                    _nativeAuthorityGeneration = 0;
                    _nativeAuthorityBound = false;
                    _nativeWorldBusActive = false;
                }
            }
            _diagnostics.Log("WorldSync",
                authorityBound
                    ? $"WS3 native authority bound: Session={sessionId:N} PeerId={peerId} " +
                      $"Role={(isHost ? "Host" : "Client")} Generation={payload.LevelGeneration} " +
                      $"WorldBus={(payload.WorldBusActive ? "Active" : "Closed")} Reason={reason}."
                    : $"WS3 native authority unbound: Session={sessionId:N} PeerId={peerId} " +
                      $"Role={(isHost ? "Host" : "Client")} Generation={payload.LevelGeneration} " +
                      $"Reason={reason}.");
            return true;
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync",
                    $"WS3 native lifecycle update failed ({reason}): {exception.Message}");
            return false;
        }
    }

    private async Task CloseNativeWorldBusAsync(string reason)
    {
        Guid sessionId;
        ulong peerId;
        bool isHost;
        int generation;
        lock (_stateGate)
        {
            if (!_nativeAuthorityBound) return;
            sessionId = _nativeAuthoritySessionId;
            peerId = _nativeAuthorityPeerId;
            isHost = _nativeAuthorityIsHost;
            generation = _nativeAuthorityGeneration;
        }
        await SetNativeWorldSyncLifecycleAsync(
            sessionId, peerId, isHost, generation, true, false, reason)
            .ConfigureAwait(false);
    }

    private async Task UnbindNativeWorldSyncAuthorityAsync(string reason)
    {
        Guid sessionId;
        ulong peerId;
        bool isHost;
        int generation;
        lock (_stateGate)
        {
            if (!_nativeAuthorityBound) return;
            sessionId = _nativeAuthoritySessionId;
            peerId = _nativeAuthorityPeerId;
            isHost = _nativeAuthorityIsHost;
            generation = _nativeAuthorityGeneration;
        }
        await SetNativeWorldSyncLifecycleAsync(
            sessionId, peerId, isHost, generation, false, false, reason)
            .ConfigureAwait(false);
    }

    private async Task TryOpenClientWorldBusAfterRecoveryAsync(string reason)
    {
        LobbyClient? client;
        WorldNetworkContext? context;
        lock (_stateGate)
        {
            client = _client;
            context = _localWorldContext;
            if (client is null || context is not { Ready: true }
                || _worldRecoveryRequired || _worldRecoveryRequestRunning
                || _worldRecoveryTargetSequence != 0 || _nativeWorldBusActive)
                return;
        }
        await SetNativeWorldSyncLifecycleAsync(
            context.SessionId, client.PeerId, false, context.WorldGeneration,
            true, true, reason).ConfigureAwait(false);
    }

    private bool AdoptHostAuthorityContextLocked(WorldAuthorityContextIdentity authorityContext)
    {
        if (_currentHostAuthorityContext is { } current && current.Matches(authorityContext))
            return false;

        if (_currentHostAuthorityContext is not null)
        {
            // Logical invalidation only. Native callbacks from the previous
            // authority incarnation may still arrive; NativeDispatchId makes them
            // exact stale results rather than sequence-order tombstones.
            _activeWorldTransaction?.Completion.TrySetResult(false);
            _activeWorldTransaction = null;
            _lastCommittedWorldSequence = 0;
            _worldRecoveryTargetSequence = 0;
            _worldRecoveryAwaitSnapshot.Clear();
            _worldRecoveryRequired = false;
            _worldRecoveryAuthorityContext = null;
            _pendingAwaitAuthorityCauses.Clear();
            _worldResumeCheckpoint = null;
        }
        _currentHostAuthorityContext = authorityContext;
        return true;
    }

    private void TrackWorldResultLocked(
        ulong nativeDispatchId,
        ulong authoritySequence,
        ClientWorldTransaction transaction)
    {
        if (nativeDispatchId == 0)
            throw new ProtocolException("NativeDispatchId zero is reserved.");
        if (!_worldResultDispatches.TryAdd(nativeDispatchId,
                new NativeWorldDispatch(nativeDispatchId, authoritySequence, transaction)))
            throw new ProtocolException($"NativeDispatchId {nativeDispatchId} is already pending.");
    }

    private bool TryTakeWorldResultLocked(
        ulong nativeDispatchId,
        out NativeWorldDispatch dispatch)
    {
        if (nativeDispatchId == 0
            || !_worldResultDispatches.Remove(nativeDispatchId, out var found))
        {
            dispatch = null!;
            return false;
        }
        dispatch = found;
        return true;
    }

    private void RemoveWorldResultDispatchLocked(ulong nativeDispatchId)
    {
        if (nativeDispatchId != 0) _worldResultDispatches.Remove(nativeDispatchId);
    }

    private void RemoveWorldResultDispatchesLocked(ClientWorldTransaction transaction)
    {
        if (_worldResultDispatches.Count == 0) return;
        var stale = _worldResultDispatches
            .Where(pair => ReferenceEquals(pair.Value.Transaction, transaction))
            .Select(pair => pair.Key)
            .ToArray();
        foreach (var nativeDispatchId in stale)
            _worldResultDispatches.Remove(nativeDispatchId);
    }

    private sealed record WorldResumeCheckpoint(
        Guid SessionId,
        int WorldGeneration,
        WorldManifestIdentity Manifest,
        ulong LastCommittedSequence,
        bool RecoveryRequired,
        AwaitAuthorityCause[] PendingAwaitAuthorityCauses);
    private WorldResumeCheckpoint? _worldResumeCheckpoint;
    private ulong _lastCommittedWorldSequence;
    private ulong _worldRecoveryTargetSequence;
    private readonly HashSet<AwaitAuthorityCause> _worldRecoveryAwaitSnapshot = new();
    private bool _worldRecoveryRequired;
    private bool _worldRecoveryRequestRunning;
    private int _disposed;

    internal MultiplayerSessionCoordinator(
        NativeRuntimeController controller,
        LauncherSettingsStore settingsStore,
        ScaleformBridgeListener bridge)
    {
        _controller = controller ?? throw new ArgumentNullException(nameof(controller));
        _settingsStore = settingsStore ?? throw new ArgumentNullException(nameof(settingsStore));
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
        _diagnostics = controller.Diagnostics;
        _bridge.MessageReceived += OnBridgeMessage;
        _controller.WorldIntentProduced += OnNativeWorldIntentProduced;
        _controller.WorldMutationCompleted += OnNativeWorldMutationCompleted;
        _controller.PlayerSyncStateProduced += OnNativePlayerSyncStateProduced;
    }

    internal event EventHandler<MultiplayerSessionView>? StateChanged;

    internal MultiplayerSessionView State
    {
        get { lock (_stateGate) return CreateView(); }
    }

    internal Task<GameCommandResultPayload> ExecuteLauncherCommandAsync(
        string command,
        IReadOnlyDictionary<string, string> arguments)
    {
        if (string.Equals(command, LauncherGameCommandNames.SpawnConnectedPlayers,
                StringComparison.Ordinal))
            return RequestRemotePlayerSpawnsAsync();
        if (string.Equals(command, LauncherGameCommandNames.WorldNetProbe,
                StringComparison.Ordinal))
            return RequestWorldNetProbeAsync(arguments);
        if (string.Equals(command, LauncherGameCommandNames.ProvisionRemoteEquipment,
                StringComparison.Ordinal))
            return ProvisionRemoteEquipmentManualAsync(arguments);
        throw new InvalidOperationException("The launcher command is not supported.");
    }

    internal void UpdateLocalRuntime(
        NativeRuntimeSnapshot runtime,
        bool positionAvailable,
        LocalPlayerPosition position,
        bool yawAvailable,
        float yaw,
        bool movementProfileAvailable,
        LocalPlayerMovementProfile movementProfile)
    {
        var now = DateTimeOffset.UtcNow;
        InvalidateWorldContextForRuntimeGeneration(runtime.LevelGeneration);
        UpdateObservedTransform(
            now, positionAvailable, position, yawAvailable, yaw,
            movementProfileAvailable, movementProfile);
        if (TryQueueLocalPosition(now)) _ = PumpLocalPositionsAsync();
        if (TryQueueLocalEquipment(now)) _ = PumpLocalEquipmentAsync();
        _ = TryAdvanceGameplayAsync(runtime);
        _ = TryConvergeRemoteCharactersAsync(runtime, now);
        _ = TryAdvanceRemoteMovementAsync(runtime, now);
    }

    private void UpdateObservedTransform(
        DateTimeOffset now,
        bool positionAvailable,
        LocalPlayerPosition position,
        bool yawAvailable,
        float yaw,
        bool movementProfileAvailable,
        LocalPlayerMovementProfile movementProfile)
    {
        lock (_stateGate)
        {
            _positionAvailable = positionAvailable;
            if (positionAvailable)
            {
                UpdateLocalMovementSpeed(now, position);
                _position = position;
            }
            else
            {
                _hasPreviousMovementSample = false;
                _observedLocalPlanarSpeed = 0F;
            }
            if (yawAvailable) _yaw = yaw;
            if (movementProfileAvailable)
            {
                _moveType = movementProfile.ResolveEffectiveMoveType(
                    _observedLocalPlanarSpeed);
                // Use transform speed only to identify the effective gait. The
                // locomotion magnitude itself comes from the game's retail
                // move_type table so timer jitter cannot modulate the remote
                // Character every 16 ms. Position feedback in the v10.6
                // follower remains responsible for the small catch-up/brake.
                _nativeMoveSpeed = movementProfile.SpeedFor(_moveType);
            }
            _gameplayReadiness.Observe(now, positionAvailable, position);
        }
    }

    private void UpdateLocalMovementSpeed(
        DateTimeOffset now,
        LocalPlayerPosition position)
    {
        if (!_hasPreviousMovementSample)
        {
            _previousMovementPosition = position;
            _previousMovementSampleAtUtc = now;
            _hasPreviousMovementSample = true;
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        var seconds = (float)(now - _previousMovementSampleAtUtc).TotalSeconds;
        var dx = position.X - _previousMovementPosition.X;
        var dz = position.Z - _previousMovementPosition.Z;
        var distance = MathF.Sqrt(dx * dx + dz * dz);
        _previousMovementPosition = position;
        _previousMovementSampleAtUtc = now;

        if (!float.IsFinite(seconds) || seconds is < 0.005F or > 0.25F
            || !float.IsFinite(distance) || distance >= 2.0F)
        {
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        var instantaneous = distance / seconds;
        if (!float.IsFinite(instantaneous) || instantaneous > 8.0F)
        {
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        // Fast attack so RUN is published in one or two samples, gentler
        // release so timer jitter cannot make the gait flap WALK/RUN every tick.
        var alpha = instantaneous >= _observedLocalPlanarSpeed ? 0.80F : 0.45F;
        _observedLocalPlanarSpeed +=
            (instantaneous - _observedLocalPlanarSpeed) * alpha;
        if (_observedLocalPlanarSpeed < 0.02F) _observedLocalPlanarSpeed = 0F;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        _bridge.MessageReceived -= OnBridgeMessage;
        _controller.WorldIntentProduced -= OnNativeWorldIntentProduced;
        _controller.WorldMutationCompleted -= OnNativeWorldMutationCompleted;
        _controller.PlayerSyncStateProduced -= OnNativePlayerSyncStateProduced;
        StopNetworkAsync("Launcher closed").GetAwaiter().GetResult();
        _commandGate.Dispose();
        _spawnGate.Dispose();
        _remoteEquipmentProvisionGate.Dispose();
        _lifetime.Dispose();
    }

    private void OnBridgeMessage(object? sender, ScaleformBridgeEvent value)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        _ = HandleBridgeMessageAsync(value.Message);
    }

    private async Task HandleBridgeMessageAsync(ScaleformBridgeMessage message)
    {
        await _commandGate.WaitAsync().ConfigureAwait(false);
        try { await DispatchBridgeMessageAsync(message).ConfigureAwait(false); }
        catch (Exception exception) { await ReportBridgeFailureAsync(exception).ConfigureAwait(false); }
        finally { _commandGate.Release(); }
    }

    private Task DispatchBridgeMessageAsync(ScaleformBridgeMessage message) =>
        message.Type.Trim().ToUpperInvariant() switch
        {
            "HELLO" => HandleHelloAsync(message),
            "PICKUP_OVERLAY_LOADED" => MarkGameplayUiLoadedAsync(message),
            "CREATE_LOBBY" => CreateLobbyAsync(message),
            "JOIN_DIRECT_IP" => JoinDirectAsync(),
            "READY" => SetReadyAsync(message.Get("ready", "0") == "1"),
            "START_GAME" => StartGameAsync(message),
            "START_GAME_ACK" => LogOnlyAsync("Start-game UI acknowledgement received."),
            "CHARACTER_SELECTED" => LogCharacterSelectionAsync(message),
            "REFRESH_SERVERS" or "JOIN_SERVER" => HandleBrowserCommandAsync(message),
            "LEAVE" => StopNetworkAsync("Lobby left from the game UI"),
            _ => LogOnlyAsync($"No multiplayer action is attached to {message.Type}.")
        };

    private Task HandleBrowserCommandAsync(ScaleformBridgeMessage message) =>
        string.Equals(message.Type, "REFRESH_SERVERS", StringComparison.OrdinalIgnoreCase)
            ? SendEmptyServerListAsync()
            : SendErrorAsync("Server discovery is disabled; use Direct Connection.");

    private async Task HandleHelloAsync(ScaleformBridgeMessage message)
    {
        await SendConfigAsync().ConfigureAwait(false);
        await SendCurrentLobbyAsync().ConfigureAwait(false);
        var source = message.Get("source", "frontend");
        var phase = message.Get("phase", "unknown");
        if (IsGameplaySource(source, phase)) await MarkGameplayUiLoadedAsync(message).ConfigureAwait(false);
        else await SendStatusAsync("Launcher bridge ready").ConfigureAwait(false);
    }

    private static bool IsGameplaySource(string source, string phase) =>
        string.Equals(source, "pickup_overlay", StringComparison.OrdinalIgnoreCase)
        || string.Equals(phase, "gameplay", StringComparison.OrdinalIgnoreCase);

    private async Task MarkGameplayUiLoadedAsync(ScaleformBridgeMessage message)
    {
        MarkGameplayUiLoaded(DateTimeOffset.UtcNow);
        _diagnostics.Log("Multiplayer",
            "Gameplay UI loaded; waiting for a stable local player position.");
        await SendStatusAsync("Waiting for local player spawn").ConfigureAwait(false);
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private void MarkGameplayUiLoaded(DateTimeOffset now)
    {
        lock (_stateGate)
        {
            if (!_uiGameplayLoaded) _gameplayReadiness.MarkGameplayUiLoaded(now);
            _uiGameplayLoaded = true;
        }
    }

    private async Task CreateLobbyAsync(ScaleformBridgeMessage message)
    {
        await StopNetworkAsync("Creating a new lobby").ConfigureAwait(false);
        var settings = _settingsStore.Load();
        var options = CreateHostOptions(settings, message);
        var host = await StartHostAsync(options).ConfigureAwait(false);
        _diagnostics.Log("Multiplayer", $"Lobby hosted on port {host.BoundPort}.");
        await SendStatusAsync("Lobby created").ConfigureAwait(false);
    }

    private async Task<LobbyHost> StartHostAsync(LobbyHostOptions options)
    {
        var host = new LobbyHost(options);
        AttachHost(host);
        lock (_stateGate) _host = host;
        try
        {
            host.Start();
            await SetNativeWorldSyncLifecycleAsync(
                host.SessionId, host.HostPeerId, true, 0, true, false, "lobby-host-created")
                .ConfigureAwait(false);
            return host;
        }
        catch
        {
            DetachHost(host);
            lock (_stateGate) if (ReferenceEquals(_host, host)) _host = null;
            await host.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    private static LobbyHostOptions CreateHostOptions(
        LauncherSettings settings,
        ScaleformBridgeMessage message) => new(
        ResolveServerName(settings, message),
        settings.PlayerName,
        settings.PlayerId,
        settings.Port,
        ProtocolConstants.DefaultLobbyCapacity,
        ResolveLevel(message),
        LobbyCompatibility.Create(settings));

    private static string ResolveServerName(
        LauncherSettings settings,
        ScaleformBridgeMessage message)
    {
        var requested = message.Get("serverName").Trim();
        var fallback = $"{settings.PlayerName}'s Lobby";
        return requested.Length is >= 1 and <= ProtocolConstants.MaximumServerNameLength
            ? requested : fallback;
    }

    private static string ResolveLevel(ScaleformBridgeMessage message)
    {
        var mapId = message.Get("mapId").Trim();
        if (mapId.Length is >= 1 and <= 128) return mapId;
        var mapName = message.Get("mapName").Trim();
        return mapName.Length is >= 1 and <= 128 ? mapName : "ChallengeMap16";
    }

    private async Task JoinDirectAsync()
    {
        await StopNetworkAsync("Joining a direct lobby").ConfigureAwait(false);
        var settings = _settingsStore.Load();
        var options = new LobbyClientOptions(settings.HostAddress, settings.Port,
            settings.PlayerId, settings.PlayerName, LobbyCompatibility.Create(settings));
        var client = await LobbyClient.ConnectAsync(options).ConfigureAwait(false);
        AttachClient(client);
        lock (_stateGate) { _client = client; _lobby = client.Snapshot; }
        client.Start();
        await SetNativeWorldSyncLifecycleAsync(
            client.SessionId, client.PeerId, false, 0, true, false, "lobby-client-joined")
            .ConfigureAwait(false);
        await PublishLobbyAsync(client.Snapshot).ConfigureAwait(false);
        await SendStatusAsync($"Connected to {settings.HostAddress}:{settings.Port}")
            .ConfigureAwait(false);
    }

    private async Task SetReadyAsync(bool ready)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null)
        {
            _diagnostics.Log("Multiplayer", "Host readiness is implicit; READY was ignored.");
            return;
        }
        if (client is null) throw new InvalidOperationException(
            "Join a lobby before changing ready state.");
        await client.SetReadyAsync(ready).ConfigureAwait(false);
        _diagnostics.Log("Multiplayer", $"Client lobby ready state: {ready}.");
    }

    private async Task StartGameAsync(ScaleformBridgeMessage message)
    {
        LobbyHost? host;
        lock (_stateGate) host = _host;
        if (host is null) throw new InvalidOperationException("Only the lobby host can start the game.");
        host.SetLevel(ResolveLevel(message));
        if (!host.CanStart) throw new InvalidOperationException(
            "Every connected client must be Ready before the game can start.");
        await host.StartGameAsync(DateTimeOffset.UtcNow.AddMilliseconds(500))
            .ConfigureAwait(false);
    }

    private async Task<GameCommandResultPayload> RequestRemotePlayerSpawnsAsync()
    {
        var target = GetSpawnCommandTarget();
        try
        {
            var hostPosition = GetLocalSpawnPosition(target.Host.HostPeerId);
            var payload = await target.Host.RequestRemotePlayerSpawnsAsync(
                target.Generation, hostPosition, _lifetime.Token).ConfigureAwait(false);
            _diagnostics.Log("Multiplayer",
                $"Host broadcast remote-player spawn request {payload.RequestId:N}.");
            return CreateSpawnBroadcastResult(payload);
        }
        finally { lock (_stateGate) _spawnCommandCollecting = false; }
    }

    private async Task<GameCommandResultPayload> RequestWorldNetProbeAsync(
        IReadOnlyDictionary<string, string> arguments)
    {
        if (!arguments.TryGetValue("manifestPath", out var manifestPath)
            || string.IsNullOrWhiteSpace(manifestPath))
            throw new InvalidOperationException("Select the AIMPWM manifest to probe.");

        LobbyHost host;
        LobbyStartPayload start;
        HashSet<ulong> expectedPeers;
        lock (_stateGate)
        {
            host = _host ?? throw new InvalidOperationException(
                "Only the lobby host can execute WORLD_NET_PROBE.");
            start = _start ?? throw new InvalidOperationException(
                "Start the multiplayer session before WORLD_NET_PROBE.");
            if ((_lobby?.Players.Count ?? 0) < 2)
                throw new InvalidOperationException("At least one remote player must be connected.");
            if (!_localGameplayReady || !_remoteGameplayReady)
                throw new InvalidOperationException(
                    "Wait until every connected player is ready in gameplay before WORLD_NET_PROBE.");
            expectedPeers = _lobby!.Players
                .Where(player => player.PeerId != host.HostPeerId)
                .Select(player => player.PeerId)
                .ToHashSet();
        }
        var runtime = _controller.Snapshot;
        if (!runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration <= 0)
            throw new InvalidOperationException(
                "Wait until the host DLL and engine thread are ready before WORLD_NET_PROBE.");

        var prepared = WorldManifestSessionLoader.Load(manifestPath);
        var load = await _controller.ExecuteCommandAsync(
            GameCommandNames.WorldManifestLoad,
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["manifestPath"] = prepared.Path
            },
            _lifetime.Token).ConfigureAwait(false);
        if (!load.Executed)
            throw new InvalidOperationException(
                $"WORLD_MANIFEST_LOAD failed: {load.StatusCode} - {load.Message}");

        WorldNetworkContext context;
        lock (_stateGate)
        {
            var sameWorld = _localWorldContext is { Ready: true } current
                && _localWorldRuntimeGeneration == runtime.LevelGeneration
                && current.Manifest == prepared.Identity
                && string.Equals(current.ManifestSha256, prepared.Sha256,
                    StringComparison.OrdinalIgnoreCase);
            if (!sameWorld)
                _worldSessionGeneration = checked(_worldSessionGeneration + 1);
            context = new WorldNetworkContext(
                host.SessionId,
                host.HostPeerId,
                _worldSessionGeneration,
                prepared.Identity,
                prepared.Sha256,
                true);
            _localWorldContext = context;
            _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
            _pendingWorldProbeResult = new TaskCompletionSource<WorldMutationResult>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingWorldProbeAckResult =
                new TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingWorldProbeExpectedPeers = expectedPeers;
            _pendingWorldProbeAcks.Clear();
            _pendingWorldProbeSequence = 0;
        }
        try
        {
            await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false);
            await host.WaitForWorldCompatibilityAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);

            var probe = await _controller.ExecuteCommandAsync(
                GameCommandNames.WorldNetProbe,
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["entityId"] = prepared.ProbeEntity.ToString()
                },
                _lifetime.Token).ConfigureAwait(false);
            if (!probe.Executed)
                throw new InvalidOperationException(
                    $"Native WORLD_NET_PROBE failed: {probe.StatusCode} - {probe.Message}");

            Task<WorldMutationResult> pending;
            Task<IReadOnlyList<WorldNetworkMutationAck>> pendingAcks;
            lock (_stateGate)
            {
                pending = (_pendingWorldProbeResult ?? throw new InvalidOperationException(
                    "WORLD_NET_PROBE local completion was lost.")).Task;
                pendingAcks = (_pendingWorldProbeAckResult ?? throw new InvalidOperationException(
                    "WORLD_NET_PROBE remote acknowledgement tracking was lost.")).Task;
            }
            var result = await pending.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);
            var acks = await pendingAcks.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);
            var localAccepted = result.Code == WorldMutationResultCode.Accepted
                && result.EntityResolved && result.Dispatched;
            var remoteAccepted = acks.Count(ack =>
                ack.Result.Code == WorldMutationResultCode.Accepted
                && ack.Result.EntityResolved && ack.Result.Dispatched);
            var success = localAccepted && remoteAccepted == expectedPeers.Count;
            var status = success ? "WORLD_NET_PROBE_COMPLETE" : "WORLD_NET_PROBE_REJECTED";
            var message =
                $"sent=1 localAccepted={(localAccepted ? 1 : 0)} sequence={result.Sequence} " +
                $"localResolved={(result.EntityResolved ? 1 : 0)} localResult={result.Code} " +
                $"remoteExpected={expectedPeers.Count} remoteReceived={acks.Count} " +
                $"remoteAccepted={remoteAccepted} remoteResolved={acks.Count(ack => ack.Result.EntityResolved)}. " +
                "No environment gameplay was executed.";
            return new GameCommandResultPayload(
                ProtocolConstants.ExperimentalSchemaVersion,
                Guid.NewGuid(),
                _controller.Snapshot.LevelGeneration,
                Guid.Empty,
                LauncherGameCommandNames.WorldNetProbe,
                status,
                message,
                success);
        }
        finally
        {
            ClearPendingWorldProbeState();
        }
    }

    private void OnNativeWorldIntentProduced(object? sender, WorldIntent intent) =>
        _ = ForwardNativeWorldIntentAsync(intent);

    private async Task ForwardNativeWorldIntentAsync(WorldIntent intent)
    {
        ulong awaitAuthorityObservationId = 0;
        if (intent.Operation == WorldReplicationConstants.CompositePublicInputOperation
            && WorldPublicActionCodec.TryDecode(intent.Payload, out var outboundRoot)
            && outboundRoot.RetailRoot
            && (outboundRoot.Flags & WorldPublicActionCodec.AwaitAuthority) != 0)
        {
            awaitAuthorityObservationId = outboundRoot.ObservationId;
        }

        // A native ROOT emitted while the authoritative World bus is closed was
        // never admitted into a multiplayer transaction. Retire its local
        // observation/reservation without turning that lifecycle condition into
        // a Host rejection or recovery-required event.
        WorldBusLifecycleState readinessState;
        lock (_stateGate)
        {
            if (!_worldSyncEnabledForSession) return;
            readinessState = GetWorldBusLifecycleStateLocked();
        }
        if (readinessState != WorldBusLifecycleState.Active)
        {
            await AbandonUnengagedPublicRootAsync(
                intent, $"world-bus-{readinessState}").ConfigureAwait(false);
            return;
        }

        var handedToTransport = false;
        var retiredWithoutTransport = false;
        var gateHeld = false;
        var receivedAt = Environment.TickCount64;
        var deadline = receivedAt + 10000;
        var pendingCount = Interlocked.Increment(ref _queuedWorldIntents);
        try
        {
            if (pendingCount > 256)
                throw new ProtocolException("WS3 authority queue full; resynchronization required.");
            gateHeld = await _worldIntentGate.WaitAsync(10000, _lifetime.Token).ConfigureAwait(false);
            if (!gateHeld || Environment.TickCount64 >= deadline)
                throw new ProtocolException("WS3 authority wait expired; resynchronization required.");
            LobbyHost? host;
            LobbyClient? client;
            LobbyStartPayload? start;
            WorldNetworkContext? context;
            while (true)
            {
                lock (_stateGate)
                {
                    host = _host;
                    client = _client;
                    start = _start;
                    context = _localWorldContext;
                    if (!_worldSyncEnabledForSession)
                        throw new WorldBusClosedException(
                            "WorldSync was disabled while a ROOT was awaiting transport.");
                    if (GetWorldBusLifecycleStateLocked() != WorldBusLifecycleState.Active)
                        throw new WorldBusClosedException(
                            $"World bus left Active while a ROOT was awaiting transport ({GetWorldBusLifecycleStateLocked()}).");
                }
                if (intent.LevelGeneration != _controller.Snapshot.LevelGeneration)
                    throw new WorldBusClosedException(
                        "Native WorldIntent generation became stale before transport handoff.");
                if (start is not null && context is { Ready: true }
                    && (host is null || host.AreWorldPeersCompatible())) break;
                if (Environment.TickCount64 >= deadline)
                    throw new ProtocolException("WS3 authority/context unavailable at deadline; resynchronization required.");
                await Task.Delay(50, _lifetime.Token).ConfigureAwait(false);
            }
            if (context is null)
                throw new WorldBusClosedException("WorldContext disappeared before transport handoff.");
            var peerId = host?.HostPeerId ?? client?.PeerId ?? 0;
            if (peerId == 0 || intent.Manifest != context.Manifest)
                throw new WorldBusClosedException(
                    "Native WorldIntent context became stale or incompatible before transport handoff.");
            AwaitAuthorityCause? awaitAuthorityCause = null;
            if (awaitAuthorityObservationId != 0)
            {
                awaitAuthorityCause = new AwaitAuthorityCause(peerId, awaitAuthorityObservationId);
                lock (_stateGate) _pendingAwaitAuthorityCauses.Add(awaitAuthorityCause.Value);
            }
            if (intent.Operation == WorldReplicationConstants.CompositePublicInputOperation
                && WorldPublicActionCodec.TryDecode(intent.Payload, out var action) && action.RetailRoot)
            {
                var payload = intent.Payload.ToArray();
                var delay = Math.Max(0.0, action.RelativeDelaySeconds -
                    (Environment.TickCount64 - receivedAt) / 1000.0);
                System.Buffers.Binary.BinaryPrimitives.WriteDoubleLittleEndian(payload.AsSpan(32, 8), delay);
                intent = intent with { Payload = payload };
            }
            var authenticated = intent with { SourcePeer = peerId };
            var envelope = new WorldNetworkIntent(
                context.SessionId,
                context.WorldGeneration,
                authenticated);
            if (host is not null)
            {
                handedToTransport = true;
                await host.SubmitWorldIntentAsync(envelope, _lifetime.Token).ConfigureAwait(false);
            }
            else if (client is not null)
            {
                handedToTransport = true;
                await client.SendWorldIntentAsync(envelope, _lifetime.Token).ConfigureAwait(false);
                if (awaitAuthorityCause is { } pendingCause)
                    _ = WatchAwaitAuthorityAsync(client, context, pendingCause);
            }
            else
                throw new ProtocolException("No active multiplayer transport exists for WorldIntent.");
        }
        catch (WorldBusClosedException exception)
        {
            if (!_lifetime.IsCancellationRequested)
                await AbandonUnengagedPublicRootAsync(intent, exception.Message).ConfigureAwait(false);
            // The item was never handed to the multiplayer transport, so the
            // explicit abandonment above is its complete disposition.
            retiredWithoutTransport = true;
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
            {
                LogWorldIntentReject(exception.Message);
                // A client write failure after handoff is ambiguous: the Host may
                // already have accepted the intent. Never "release" an
                // AwaitAuthority takeover based on that ambiguity; ask the Host
                // journal from the durable checkpoint instead.
                LobbyClient? recoveryClient;
                WorldNetworkContext? recoveryContext;
                lock (_stateGate)
                {
                    recoveryClient = handedToTransport && awaitAuthorityObservationId != 0
                        ? _client : null;
                    recoveryContext = recoveryClient is not null ? _localWorldContext : null;
                }
                if (recoveryClient is not null && recoveryContext is { Ready: true })
                    _ = RequestWorldRecoveryAsync(recoveryClient, recoveryContext, "ambiguous-intent-send");
            }
        }
        finally
        {
            // Native reserves before fan-out. Every local early return must
            // retire that reservation, including a closed compatibility barrier.
            // Once handed to transport, only an explicit authority rejection is
            // conclusive: an I/O failure may follow acceptance and an echo in flight.
            try
            {
                if (!handedToTransport && !retiredWithoutTransport)
                    await ReleaseRejectedPublicRootAsync(intent).ConfigureAwait(false);
            }
            finally
            {
                if (gateHeld) _worldIntentGate.Release();
                Interlocked.Decrement(ref _queuedWorldIntents);
            }
        }
    }

    private sealed class WorldBusClosedException : InvalidOperationException
    {
        internal WorldBusClosedException(string message) : base(message) { }
    }

    private void LogWorldIntentAbandon(string reason)
    {
        var now = DateTimeOffset.UtcNow;
        int count;
        string lastReason;
        bool shouldLog;
        lock (_stateGate)
        {
            _suppressedWorldIntentAbandons++;
            _lastWorldIntentAbandonReason = reason;
            shouldLog = now >= _nextWorldIntentAbandonSummaryAtUtc;
            count = shouldLog ? _suppressedWorldIntentAbandons : 0;
            lastReason = _lastWorldIntentAbandonReason;
            if (shouldLog)
            {
                _suppressedWorldIntentAbandons = 0;
                _nextWorldIntentAbandonSummaryAtUtc = now.AddSeconds(2);
            }
        }
        if (shouldLog)
            _diagnostics.Log("WorldSync",
                $"WS3 pre-bus ROOTs abandoned without authority rejection: count={count} last={lastReason}");
    }

    private async Task AbandonUnengagedPublicRootAsync(WorldIntent intent, string reason)
    {
        if (intent.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || intent.RequestedKind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(intent.Payload, out _, out _, out var retailRoot)
            || !retailRoot) return;
        LogWorldIntentAbandon(reason);
        try
        {
            await _controller.SendWorldIntentAbandonedAsync(intent, _lifetime.Token)
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync",
                    $"Pre-bus ROOT observation could not be retired: {exception.Message}");
        }
    }

    private async Task WatchAwaitAuthorityAsync(
        LobbyClient client,
        WorldNetworkContext context,
        AwaitAuthorityCause cause)
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(5), _lifetime.Token).ConfigureAwait(false);
            lock (_stateGate)
            {
                if (_worldRecoveryRequired
                    || !_pendingAwaitAuthorityCauses.Contains(cause)
                    || _localWorldContext is not { Ready: true } current
                    || current.SessionId != context.SessionId
                    || current.WorldGeneration != context.WorldGeneration)
                    return;
            }
            await RequestWorldRecoveryAsync(client, context, "AwaitAuthority-timeout")
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
        }
    }

    private void OnWorldIntentRejected(object? sender, WorldNetworkIntent intent) =>
        _ = ReleaseRejectedPublicRootAsync(intent.Intent);

    private void LogWorldIntentReject(string reason)
    {
        var now = DateTimeOffset.UtcNow;
        int count;
        string lastReason;
        bool shouldLog;
        lock (_stateGate)
        {
            _suppressedWorldIntentRejects++;
            _lastWorldIntentRejectReason = reason;
            shouldLog = now >= _nextWorldIntentRejectSummaryAtUtc;
            count = shouldLog ? _suppressedWorldIntentRejects : 0;
            lastReason = _lastWorldIntentRejectReason;
            if (shouldLog)
            {
                _suppressedWorldIntentRejects = 0;
                _nextWorldIntentRejectSummaryAtUtc = now.AddSeconds(2);
            }
        }
        if (shouldLog)
        {
            _diagnostics.Log("WorldSync",
                $"WorldIntent transport rejects summarized: count={count} last={lastReason}");
        }
    }

    private async Task ReleaseRejectedPublicRootAsync(WorldIntent intent)
    {
        if (intent.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || intent.RequestedKind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(intent.Payload, out _, out _, out var retailRoot)
            || !retailRoot) return;
        try
        {
            if (WorldPublicActionCodec.TryDecode(intent.Payload, out var action))
            {
                var awaitAuthority = (action.Flags & WorldPublicActionCodec.AwaitAuthority) != 0;
                lock (_stateGate)
                {
                    if (awaitAuthority)
                    {
                        var sourcePeer = intent.SourcePeer != 0
                            ? intent.SourcePeer
                            : _client?.PeerId ?? _host?.HostPeerId ?? 0;
                        if (sourcePeer != 0)
                            _pendingAwaitAuthorityCauses.Add(
                                new AwaitAuthorityCause(sourcePeer, action.ObservationId));
                    }
                    // An explicit Host rejection is conclusive. AwaitAuthority
                    // has no local World effect to roll back; a non-Await ROOT
                    // may already have executed optimistically. Neither case can
                    // continue as if the authoritative transaction existed.
                    _worldRecoveryRequired = true;
                }
                _diagnostics.Log("WorldSync", awaitAuthority
                    ? $"WS3 cause {action.ObservationId:X16} was not accepted; its World effect has not executed. Rejoin/resynchronization is required; no delayed CATHODE replay is attempted."
                    : $"WS3 cause {action.ObservationId:X16} was rejected after optimistic local execution may have occurred. Rejoin/resynchronization is required; no automatic replay or rollback is attempted.");
            }
            // A conclusive authority rejection is terminal for this World bus
            // generation. Close native capture immediately so one rejection
            // cannot snowball into thousands of additional proposals while the
            // Coordinator is already recovery-required.
            await CloseNativeWorldBusAsync("host-intent-rejected").ConfigureAwait(false);
            await _controller.SendWorldIntentRejectedAsync(intent, _lifetime.Token)
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"Rejected ROOT observation could not be released: {exception.Message}");
        }
    }

    private void OnWorldMutationAccepted(object? sender, WorldHostAcceptedMutation accepted)
    {
        bool changed;
        lock (_stateGate) changed = AdoptHostAuthorityContextLocked(accepted.AuthorityContext);
        if (changed)
            _diagnostics.Log("WorldSync",
                $"WS3 Host authority context adopted: generation={accepted.AuthorityContext.WorldGeneration} " +
                $"epoch={accepted.AuthorityContext.Epoch} sha256={accepted.AuthorityContext.ManifestSha256}.");
        _ = ApplyNetworkWorldMutationAsync(accepted.Mutation, accepted.AuthorityContext);
    }

    private void OnWorldMutationReceived(object? sender, WorldNetworkMutation mutation) =>
        _ = ApplyNetworkWorldMutationAsync(mutation, null);

    private static bool TryDescribeAuthorityTransaction(
        WorldNetworkMutation networkMutation,
        out ulong startSequence,
        out int rootCount,
        out int rootOrdinal)
    {
        startSequence = networkMutation.Mutation.Sequence;
        rootCount = 1;
        rootOrdinal = 0;
        if (networkMutation.Mutation.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || networkMutation.Mutation.Kind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(networkMutation.Mutation.Payload, out var root)
            || !root.RetailRoot)
            return true;
        if (root.AuthorityTransaction == 0
            || networkMutation.Mutation.Sequence != root.AuthorityTransaction + root.RootOrdinal)
            return false;
        startSequence = root.AuthorityTransaction;
        rootCount = root.RootCount;
        rootOrdinal = root.RootOrdinal;
        return true;
    }

    private async Task ApplyNetworkWorldMutationAsync(
        WorldNetworkMutation networkMutation,
        WorldAuthorityContextIdentity? hostAuthorityContext)
    {
        await _worldMutationApplyGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        ClientWorldTransaction? transaction = null;
        var lastMember = false;
        try
        {
            lock (_stateGate)
            {
                if (!_worldSyncEnabledForSession) return;
            }
            WorldNetworkContext? context;
            LobbyStartPayload? start;
            LobbyClient? client;
            LobbyHost? host;
            lock (_stateGate)
            {
                context = _localWorldContext;
                start = _start;
                client = _client;
                host = _host;
                if (_pendingWorldProbeResult is not null
                    && networkMutation.Mutation.Operation == WorldReplicationConstants.NetworkProbeOperation)
                    _pendingWorldProbeSequence = networkMutation.Mutation.Sequence;
            }
            if (context is null || !context.Ready || start is null
                || networkMutation.SessionId != context.SessionId
                || networkMutation.WorldGeneration != context.WorldGeneration
                || networkMutation.Mutation.Manifest != context.Manifest)
                throw new ProtocolException("Authoritative WorldMutation context is incompatible.");
            if (hostAuthorityContext is { } expectedHostContext
                && (host is null
                    || !expectedHostContext.Matches(context)
                    || !host.IsCurrentWorldAuthorityContext(expectedHostContext)))
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host mutation dispatch ignored before native enqueue: " +
                    $"sequence={networkMutation.Mutation.Sequence} generation={expectedHostContext.WorldGeneration} " +
                    $"epoch={expectedHostContext.Epoch}.");
                return;
            }
            var runtimeGeneration = _controller.Snapshot.LevelGeneration;
            if (runtimeGeneration <= 0 || runtimeGeneration != _localWorldRuntimeGeneration)
                throw new ProtocolException("Local LevelGeneration changed before WorldMutation dispatch.");
            if (!TryDescribeAuthorityTransaction(networkMutation,
                    out var transactionStart, out var transactionRoots, out var rootOrdinal))
                throw new ProtocolException("Authoritative transaction metadata is invalid.");

            var requestRecovery = false;
            lock (_stateGate)
            {
                if (networkMutation.Mutation.Sequence <= _lastCommittedWorldSequence)
                    return; // clean committed retransmission
                if (_worldRecoveryRequired)
                {
                    if (hostAuthorityContext is { } currentHostAuthority
                        && _worldRecoveryAuthorityContext is { } failedHostAuthority
                        && !failedHostAuthority.Matches(currentHostAuthority))
                    {
                        _worldRecoveryRequired = false;
                        _worldRecoveryAuthorityContext = null;
                    }
                    else
                    {
                        return; // fail-stop until a new authority context is established
                    }
                }

                transaction = _activeWorldTransaction;
                if (transaction is not null
                    && hostAuthorityContext is { } incomingHostContext
                    && transaction.HostAuthorityContext is { } activeHostContext
                    && !activeHostContext.Matches(incomingHostContext))
                {
                    transaction.Completion.TrySetResult(false);
                    _activeWorldTransaction = null;
                    transaction = null;
                }
                if (transaction is not null && transaction.ReceivedCount < transaction.RootCount)
                {
                    var exactNext = transaction.StartSequence == transactionStart
                        && transaction.RootCount == transactionRoots
                        && rootOrdinal == transaction.ReceivedCount
                        && networkMutation.Mutation.Sequence == transaction.StartSequence + checked((ulong)rootOrdinal);
                    var freshPrefix = transaction.StartSequence == transactionStart
                        && transaction.RootCount == transactionRoots
                        && rootOrdinal == 0
                        && networkMutation.Mutation.Sequence == transactionStart;
                    if (freshPrefix && transaction.ReceivedCount != 0)
                    {
                        // The journal may retransmit ordinal-0 while an older
                        // prefix is still buffered. No gameplay can have run from
                        // an incomplete prefix, so replace the assembly in place.
                        // Do not erase native-dispatch correlation here. The
                        // replaced prefix may still emit a late result, and its
                        // sequence can be reused by the fresh prefix/context.
                        transaction.Completion.TrySetResult(false);
                        transaction = new ClientWorldTransaction(
                            transactionStart, transactionRoots, context, hostAuthorityContext);
                        _activeWorldTransaction = transaction;
                    }
                    else if (!exactNext)
                    {
                        requestRecovery = true;
                    }
                }
                if (transaction is null)
                {
                    if (transactionStart != _lastCommittedWorldSequence + 1U || rootOrdinal != 0)
                    {
                        // Never start from a suffix. A retransmitted ordinal-0
                        // from the Host journal is the only safe way to rebuild an
                        // uncommitted transaction.
                        requestRecovery = true;
                    }
                    else
                    {
                        transaction = new ClientWorldTransaction(
                            transactionStart, transactionRoots, context, hostAuthorityContext);
                        _activeWorldTransaction = transaction;
                    }
                }
                if (!requestRecovery && transaction is not null)
                {
                    if (transaction.StartSequence != transactionStart
                        || transaction.RootCount != transactionRoots
                        || rootOrdinal != transaction.ReceivedCount
                        || networkMutation.Mutation.Sequence != transaction.StartSequence + checked((ulong)rootOrdinal))
                        throw new ProtocolException("Authoritative transaction ordering is not contiguous.");
                    if (networkMutation.Mutation.Operation == WorldReplicationConstants.CompositePublicInputOperation
                        && WorldPublicActionCodec.TryDecode(networkMutation.Mutation.Payload, out var observedRoot)
                        && observedRoot.RetailRoot
                        && (observedRoot.Flags & WorldPublicActionCodec.AwaitAuthority) != 0)
                    {
                        var cause = new AwaitAuthorityCause(
                            networkMutation.Mutation.SourcePeer, observedRoot.ObservationId);
                        if (_pendingAwaitAuthorityCauses.Contains(cause))
                            transaction.LocalAwaitCauses.Add(cause);
                    }
                    transaction.ReceivedCount++;
                    lastMember = transaction.ReceivedCount == transaction.RootCount;
                }
            }
            if (requestRecovery)
            {
                if (client is not null)
                    _ = RequestWorldRecoveryAsync(client, context, "sequence-gap");
                else
                    MarkWorldRecoveryRequired(
                        "Host local authority sequence gap.", transactionStart,
                        hostAuthorityContext);
                return;
            }

            if (transaction is null)
                throw new ProtocolException("Authoritative transaction tracker disappeared before native dispatch.");

            // Re-check immediately before native enqueue. A context reset can
            // happen while this async task waited on the apply gate.
            if (hostAuthorityContext is { } dispatchHostContext
                && (host is null || !host.IsCurrentWorldAuthorityContext(dispatchHostContext)))
            {
                lock (_stateGate)
                {
                    transaction.Completion.TrySetResult(false);
                    if (ReferenceEquals(_activeWorldTransaction, transaction))
                        _activeWorldTransaction = null;
                }
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host mutation dispatch ignored at native enqueue: " +
                    $"sequence={networkMutation.Mutation.Sequence} generation={dispatchHostContext.WorldGeneration} " +
                    $"epoch={dispatchHostContext.Epoch}.");
                return;
            }

            var localMutation = networkMutation.Mutation with
            {
                LevelGeneration = checked((uint)runtimeGeneration)
            };
            var nativeDispatchId = _controller.ReserveWorldMutationDispatchId();
            lock (_stateGate)
                TrackWorldResultLocked(
                    nativeDispatchId, networkMutation.Mutation.Sequence, transaction);
            try
            {
                await _controller.SendWorldMutationAsync(
                        localMutation, nativeDispatchId, _lifetime.Token)
                    .ConfigureAwait(false);
            }
            catch
            {
                lock (_stateGate)
                    RemoveWorldResultDispatchLocked(nativeDispatchId);
                throw;
            }

            if (lastMember)
            {
                try
                {
                    await transaction.Completion.Task.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                        .ConfigureAwait(false);
                }
                catch (TimeoutException)
                {
                    MarkWorldRecoveryRequired(
                        $"Transaction {transaction.StartSequence} did not reach native commit before timeout.",
                        transaction.StartSequence,
                        transaction.HostAuthorityContext);
                }
            }
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException or OverflowException)
        {
            if (!_lifetime.IsCancellationRequested)
            {
                MarkWorldRecoveryRequired(
                    $"WorldMutation dispatch failed: {exception.Message}",
                    transaction?.StartSequence ?? networkMutation.Mutation.Sequence,
                    transaction?.HostAuthorityContext ?? hostAuthorityContext);
                _diagnostics.Log("WorldSync", $"WorldMutation rejected before DLL dispatch: {exception.Message}");
            }
        }
        finally
        {
            _worldMutationApplyGate.Release();
        }
    }

    private void MarkWorldRecoveryRequired(
        string reason,
        ulong failedAuthorityTransaction = 0,
        WorldAuthorityContextIdentity? hostAuthorityContext = null)
    {
        LobbyHost? host;
        lock (_stateGate) host = _host;

        // Step 4.3: a Host failure may mutate authority/local fail-stop state only
        // if the transaction's immutable authority epoch is still current.
        if (host is not null)
        {
            if (hostAuthorityContext is not { } expected
                || !host.MarkWorldAuthorityUnhealthy(
                    expected, failedAuthorityTransaction, reason))
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host fail-stop ignored: transaction={failedAuthorityTransaction} " +
                    $"reason={reason}");
                return;
            }

            lock (_stateGate)
            {
                if (_host != host || !host.IsCurrentWorldAuthorityContext(expected))
                {
                    _diagnostics.Log("WorldSync",
                        $"WS3 Host fail-stop became stale before Coordinator state update: " +
                        $"transaction={failedAuthorityTransaction} epoch={expected.Epoch}.");
                    return;
                }
                _worldRecoveryRequired = true;
                _worldRecoveryAuthorityContext = expected;
            }
        }
        else
        {
            lock (_stateGate)
            {
                _worldRecoveryRequired = true;
                _worldRecoveryAuthorityContext = null;
            }
        }

        _diagnostics.Log("WorldSync",
            $"WS3 transaction recovery stopped: {reason} Rejoin/resynchronization is required; no partially applied transaction is replayed.");
        _ = CloseNativeWorldBusAsync("recovery-required");
    }

    private async Task RequestWorldRecoveryAsync(
        LobbyClient client,
        WorldNetworkContext context,
        string reason)
    {
        ulong checkpoint;
        lock (_stateGate)
        {
            if (_worldRecoveryRequired || _worldRecoveryRequestRunning
                || _localWorldContext is not { Ready: true } current
                || current.SessionId != context.SessionId
                || current.WorldGeneration != context.WorldGeneration)
                return;
            _worldRecoveryRequestRunning = true;
            checkpoint = _lastCommittedWorldSequence;
            _worldRecoveryAwaitSnapshot.Clear();
            foreach (var cause in _pendingAwaitAuthorityCauses)
                _worldRecoveryAwaitSnapshot.Add(cause);
        }
        try
        {
            await CloseNativeWorldBusAsync($"recovery-request:{reason}")
                .ConfigureAwait(false);
            var request = new WorldTransactionRecoveryRequest(
                client.SessionId, client.PeerId, context.WorldGeneration, checkpoint);
            await client.SendWorldTransactionRecoveryRequestAsync(request, _lifetime.Token)
                .ConfigureAwait(false);
            _diagnostics.Log("WorldSync",
                $"Requested authoritative transaction recovery after committed sequence {checkpoint} ({reason}).");
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            lock (_stateGate) _worldRecoveryRequestRunning = false;
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"World recovery request failed: {exception.Message}");
        }
    }

    private void OnWorldRecoveryStatusReceived(object? sender, WorldTransactionRecoveryStatus status)
    {
        ulong committed;
        var missingAwaitAfterRecovery = false;
        lock (_stateGate)
        {
            _worldRecoveryRequestRunning = false;
            committed = _lastCommittedWorldSequence;
            if (_localWorldContext is not { Ready: true } context
                || status.SessionId != context.SessionId
                || status.WorldGeneration != context.WorldGeneration)
                return;
            if (status.Status != WorldTransactionRecoveryStatusCode.Complete)
            {
                _worldRecoveryRequired = true;
                // An unavailable/context-rejected recovery is terminal for this
                // generation. AwaitAuthority causes must not remain pending or
                // trigger delayed gameplay retry; the next safe action is rejoin
                // or a genuinely new WorldContext generation.
                _pendingAwaitAuthorityCauses.Clear();
                _worldRecoveryAwaitSnapshot.Clear();
                _worldRecoveryTargetSequence = 0;
            }
            else
            {
                _worldRecoveryTargetSequence = status.HostThroughSequence;
                if (_lastCommittedWorldSequence >= _worldRecoveryTargetSequence)
                {
                    missingAwaitAfterRecovery =
                        _worldRecoveryAwaitSnapshot.Any(_pendingAwaitAuthorityCauses.Contains);
                    if (missingAwaitAfterRecovery) _worldRecoveryRequired = true;
                    _worldRecoveryTargetSequence = 0;
                    _worldRecoveryAwaitSnapshot.Clear();
                }
            }
        }
        if (status.Status == WorldTransactionRecoveryStatusCode.Complete)
        {
            _diagnostics.Log("WorldSync",
                missingAwaitAfterRecovery
                    ? "Host journal reached the requested checkpoint but at least one pre-disconnect AwaitAuthority cause was never authority-assigned; rejoin/resynchronization is required."
                    : $"Host recovery suffix accepted: checkpoint={committed} hostThrough={status.HostThroughSequence}.");
            if (!missingAwaitAfterRecovery)
                _ = TryOpenClientWorldBusAfterRecoveryAsync("recovery-complete");
        }
        else
        {
            _diagnostics.Log("WorldSync",
                $"Host recovery unavailable ({status.Status}): {status.Reason} Rejoin/resynchronization is required.");
            _ = CloseNativeWorldBusAsync("recovery-unavailable");
        }
    }

    private bool TryCommitCoordinatorTransactionLocked(ClientWorldTransaction transaction)
    {
        if (_worldRecoveryRequired
            || transaction.StartSequence != _lastCommittedWorldSequence + 1U
            || _localWorldContext is not { Ready: true } current
            || !SameWorldContext(current, transaction.WorldContext))
            return false;

        _lastCommittedWorldSequence = transaction.EndSequence;
        foreach (var cause in transaction.LocalAwaitCauses)
            _pendingAwaitAuthorityCauses.Remove(cause);
        if (ReferenceEquals(_activeWorldTransaction, transaction))
            _activeWorldTransaction = null;
        _worldResumeCheckpoint = new WorldResumeCheckpoint(
            current.SessionId,
            current.WorldGeneration,
            current.Manifest,
            _lastCommittedWorldSequence,
            false,
            _pendingAwaitAuthorityCauses.ToArray());
        if (_worldRecoveryTargetSequence != 0
            && _lastCommittedWorldSequence >= _worldRecoveryTargetSequence)
        {
            if (_worldRecoveryAwaitSnapshot.Any(_pendingAwaitAuthorityCauses.Contains))
                _worldRecoveryRequired = true;
            _worldRecoveryTargetSequence = 0;
            _worldRecoveryAwaitSnapshot.Clear();
        }
        return !_worldRecoveryRequired;
    }

    private void OnNativeWorldMutationCompleted(object? sender, WorldMutationResult result)
    {
        LobbyHost? host;
        LobbyClient? client;
        WorldNetworkContext? context;
        ClientWorldTransaction? tracked = null;
        NativeWorldDispatch? nativeDispatch = null;
        lock (_stateGate)
        {
            host = _host;
            client = _client;
            context = _localWorldContext;
            if (TryTakeWorldResultLocked(result.NativeDispatchId, out var dequeued))
            {
                nativeDispatch = dequeued;
                tracked = dequeued.Transaction;
            }
        }

        if (tracked is not null)
        {
            // Authority/world context must be proven current before *any* result
            // semantic (including the secondary authority-sequence cross-check) may
            // mutate recovery or authority state. A stale callback is inert first.
            var staleWorldContext = context is null
                || !SameWorldContext(context, tracked.WorldContext);
            var staleHostAuthority = tracked.HostAuthorityContext is { } expectedHostContext
                && (host is null || !host.IsCurrentWorldAuthorityContext(expectedHostContext));
            if (staleWorldContext || staleHostAuthority)
            {
                lock (_stateGate)
                {
                    tracked.ResultCount++;
                    if (tracked.ResultCount >= tracked.ReceivedCount)
                        tracked.Completion.TrySetResult(false);
                    if (ReferenceEquals(_activeWorldTransaction, tracked))
                        _activeWorldTransaction = null;
                }
                var epoch = tracked.HostAuthorityContext?.Epoch ?? 0U;
                _diagnostics.Log("WorldSync",
                    $"WS3 stale native WorldMutation result ignored: dispatch={result.NativeDispatchId} " +
                    $"sequence={result.Sequence} generation={tracked.WorldContext.WorldGeneration} " +
                    $"epoch={epoch} code={result.Code} dispatched={result.Dispatched}.");
                return;
            }
        }

        if (nativeDispatch is not null && nativeDispatch.AuthoritySequence != result.Sequence)
        {
            _diagnostics.Log("WorldSync",
                $"WS3 NativeDispatchId correlation rejected sequence mismatch: " +
                $"dispatch={result.NativeDispatchId} expectedSequence={nativeDispatch.AuthoritySequence} " +
                $"resultSequence={result.Sequence}.");
            MarkWorldRecoveryRequired(
                $"Native dispatch {result.NativeDispatchId} returned sequence {result.Sequence}, expected {nativeDispatch.AuthoritySequence}.",
                tracked?.StartSequence ?? nativeDispatch.AuthoritySequence,
                tracked?.HostAuthorityContext);
            tracked?.Completion.TrySetResult(false);
            return;
        }

        if (tracked is null)
        {
            // Every Launcher->DLL WorldMutation now has an explicit local dispatch
            // identity. An unknown result can only be stale, already cleaned, or
            // produced by an incompatible native build; never bind it by Sequence.
            _diagnostics.Log("WorldSync",
                $"WS3 untracked native result ignored as stale/uncertified: " +
                $"dispatch={result.NativeDispatchId} sequence={result.Sequence} " +
                $"code={result.Code} dispatched={result.Dispatched} host={host is not null}.");
            return;
        }

        TaskCompletionSource<WorldMutationResult>? completion = null;
        ClientWorldTransaction? finishedTransaction = null;
        var transactionSucceeded = false;
        var transactionFailed = false;
        var retryIncompleteTransaction = false;
        ulong firstFailedTransaction = 0;
        WorldAuthorityContextIdentity? failedHostAuthorityContext = null;

        lock (_stateGate)
        {
            if (_pendingWorldProbeResult is not null
                && _pendingWorldProbeSequence != 0
                && _pendingWorldProbeSequence == result.Sequence)
                completion = _pendingWorldProbeResult;

            if (tracked is not null)
            {
                tracked.ResultCount++;
                var resultFailed = result.Code != WorldMutationResultCode.Accepted || !result.Dispatched;
                if (resultFailed)
                {
                    tracked.Failed = true;
                    // Only the current transaction may enter fail-stop. Host
                    // authority context was proven current above; Client state
                    // retains the Step 4 behavior directly.
                    if (tracked.ReceivedCount >= tracked.RootCount)
                    {
                        if (tracked.HostAuthorityContext is null)
                            _worldRecoveryRequired = true;
                        firstFailedTransaction = tracked.StartSequence;
                        failedHostAuthorityContext = tracked.HostAuthorityContext;
                    }
                }

                // If the authoritative transaction was never fully received, the
                // native segment cannot have crossed gameplay: the native pump
                // requires rootCount contiguous members before preflight/execution.
                if (resultFailed && tracked.ReceivedCount < tracked.RootCount)
                {
                    retryIncompleteTransaction = result.Code == WorldMutationResultCode.QueueRejected;
                    transactionFailed = !retryIncompleteTransaction;
                    if (transactionFailed && tracked.HostAuthorityContext is null)
                        _worldRecoveryRequired = true;
                    finishedTransaction = tracked;
                    if (ReferenceEquals(_activeWorldTransaction, tracked))
                        _activeWorldTransaction = null;
                    // Keep remaining dispatch correlations. Their callbacks may
                    // still arrive and must be drained under this exact context.
                }
                else if (tracked.ResultCount == tracked.RootCount)
                {
                    finishedTransaction = tracked;
                    transactionSucceeded = !tracked.Failed
                        && tracked.StartSequence == _lastCommittedWorldSequence + 1U
                        && (tracked.HostAuthorityContext is not null || !_worldRecoveryRequired);
                    if (transactionSucceeded && tracked.HostAuthorityContext is null)
                    {
                        transactionSucceeded = TryCommitCoordinatorTransactionLocked(tracked);
                        if (!transactionSucceeded)
                        {
                            _worldRecoveryRequired = true;
                            transactionFailed = true;
                        }
                    }
                    else if (!transactionSucceeded)
                    {
                        if (tracked.HostAuthorityContext is null)
                            _worldRecoveryRequired = true;
                        transactionFailed = true;
                    }
                }
            }
        }

        if (firstFailedTransaction != 0)
        {
            MarkWorldRecoveryRequired(
                $"Transaction {firstFailedTransaction} produced a failing native result before full commit ({result.Code}, dispatched={result.Dispatched}).",
                firstFailedTransaction,
                failedHostAuthorityContext);
        }

        if (finishedTransaction is not null)
        {
            lock (_stateGate)
                RemoveWorldResultDispatchesLocked(finishedTransaction);

            if (transactionSucceeded && host is not null
                && finishedTransaction.HostAuthorityContext is { } hostTransactionContext)
            {
                if (!host.ReportWorldGameplayCommitted(
                        hostTransactionContext,
                        finishedTransaction.StartSequence,
                        finishedTransaction.EndSequence,
                        out var commitRejection))
                {
                    transactionSucceeded = false;
                    if (string.Equals(commitRejection,
                            "HOST_GAMEPLAY_COMMIT_CONTEXT_STALE", StringComparison.Ordinal))
                    {
                        transactionFailed = false;
                        _diagnostics.Log("WorldSync",
                            $"WS3 stale HostGameplayCommitted result ignored: " +
                            $"transaction={finishedTransaction.StartSequence} " +
                            $"epoch={hostTransactionContext.Epoch}.");
                    }
                    else
                    {
                        transactionFailed = true;
                        MarkWorldRecoveryRequired(
                            $"HostGameplayCommitted bookkeeping failed for transaction {finishedTransaction.StartSequence}: {commitRejection}",
                            finishedTransaction.StartSequence,
                            hostTransactionContext);
                    }
                }
                else
                {
                    lock (_stateGate)
                    {
                        if (host.IsCurrentWorldAuthorityContext(hostTransactionContext))
                            transactionSucceeded = TryCommitCoordinatorTransactionLocked(finishedTransaction);
                        else
                            transactionSucceeded = false;
                    }
                    if (!transactionSucceeded)
                    {
                        _diagnostics.Log("WorldSync",
                            $"WS3 HostGameplayCommitted callback became stale before Coordinator commit: " +
                            $"transaction={finishedTransaction.StartSequence} epoch={hostTransactionContext.Epoch}.");
                    }
                }
            }

            if (transactionFailed && !retryIncompleteTransaction)
            {
                MarkWorldRecoveryRequired(
                    $"Transaction {finishedTransaction.StartSequence} was not committed by local CATHODE.",
                    finishedTransaction.StartSequence,
                    finishedTransaction.HostAuthorityContext);
            }

            finishedTransaction.Completion.TrySetResult(transactionSucceeded);
            if (transactionSucceeded)
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 transaction committed through sequence {_lastCommittedWorldSequence}.");
                if (client is not null)
                    _ = TryOpenClientWorldBusAfterRecoveryAsync("recovery-suffix-committed");
            }
            else if (retryIncompleteTransaction)
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 transaction {finishedTransaction.StartSequence} expired before complete assembly; requesting the authoritative suffix again from the last committed checkpoint.");
                if (client is not null && context is { Ready: true })
                    _ = RequestWorldRecoveryAsync(client, context, "incomplete-transaction");
                else
                    MarkWorldRecoveryRequired(
                        "Host-local transaction assembly became incomplete.",
                        finishedTransaction.StartSequence,
                        finishedTransaction.HostAuthorityContext);
            }
            else if (transactionFailed)
                _diagnostics.Log("WorldSync",
                    $"WS3 transaction {finishedTransaction.StartSequence} was not committed; rejoin/resynchronization is required and it will not be auto-replayed.");
        }

        if (result.Code is WorldMutationResultCode.NativeFault or WorldMutationResultCode.QueueRejected
            or WorldMutationResultCode.ManifestRejected or WorldMutationResultCode.GenerationRejected
            or WorldMutationResultCode.EntityRejected)
        {
            var now = DateTimeOffset.UtcNow;
            int suppressed;
            bool shouldLog;
            lock (_stateGate)
            {
                _suppressedWorldMutationFaults++;
                shouldLog = now >= _nextWorldMutationFaultSummaryAtUtc;
                suppressed = shouldLog ? _suppressedWorldMutationFaults : 0;
                if (shouldLog)
                {
                    _suppressedWorldMutationFaults = 0;
                    _nextWorldMutationFaultSummaryAtUtc = now.AddSeconds(5);
                }
            }
            if (shouldLog)
            {
                _diagnostics.Log("WorldSync",
                    $"WorldMutation faults summarized: count={suppressed} lastCode={result.Code} " +
                    $"resolved={result.EntityResolved} dispatched={result.Dispatched}. " +
                    "World state may require resynchronization from a common save; partially applied ROOTs are never retried.");
            }
        }
        completion?.TrySetResult(result);
        if (client is not null && context is not null && context.Ready
            && (tracked is null || SameWorldContext(context, tracked.WorldContext)))
        {
            var ack = new WorldNetworkMutationAck(
                client.SessionId,
                client.PeerId,
                context.WorldGeneration,
                result);
            _ = SendWorldAckBestEffortAsync(client, ack);
        }
    }

    private async Task SendWorldAckBestEffortAsync(
        LobbyClient client,
        WorldNetworkMutationAck ack)
    {
        try
        {
            await client.SendWorldMutationAckAsync(ack, _lifetime.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"WorldMutation ACK failed: {exception.Message}");
        }
    }

    private void OnWorldMutationAckReceived(object? sender, WorldNetworkMutationAck ack)
    {
        TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>? completion = null;
        IReadOnlyList<WorldNetworkMutationAck>? completedAcks = null;
        lock (_stateGate)
        {
            if (_pendingWorldProbeAckResult is not null
                && _pendingWorldProbeExpectedPeers?.Contains(ack.PeerId) == true
                && _pendingWorldProbeSequence != 0
                && ack.Result.Sequence == _pendingWorldProbeSequence)
            {
                _pendingWorldProbeAcks[ack.PeerId] = ack;
                if (_pendingWorldProbeAcks.Count == _pendingWorldProbeExpectedPeers.Count)
                {
                    completion = _pendingWorldProbeAckResult;
                    completedAcks = _pendingWorldProbeAcks.Values
                        .OrderBy(value => value.PeerId)
                        .ToArray();
                }
            }
        }
        if (completion is not null && completedAcks is not null)
            completion.TrySetResult(completedAcks);
    }

    private void OnWorldContextReceived(object? sender, WorldNetworkContext context)
    {
        if (sender is LobbyClient)
            _ = PrepareClientWorldContextAsync(context);
        else
            _diagnostics.Log("WorldSync",
                $"WorldContext observed: peer={context.PeerId} ready={context.Ready} map={context.Manifest.MapContentId}.");
    }

    private async Task PrepareClientWorldContextAsync(WorldNetworkContext hostContext)
    {
        try
        {
            lock (_stateGate)
            {
                if (!_worldSyncEnabledForSession)
                {
                    _diagnostics.Log("WorldSync",
                        "Host WorldContext ignored because WorldSync is disabled locally for this game session.");
                    return;
                }
            }
            LobbyClient client;
            lock (_stateGate)
            {
                client = _client ?? throw new InvalidOperationException("Client session is unavailable.");
                _ = _start ?? throw new InvalidOperationException("Client session has not started.");
            }
            if (!hostContext.Ready)
            {
                await CloseNativeWorldBusAsync("host-world-context-invalidated")
                    .ConfigureAwait(false);
                lock (_stateGate) ResetWorldReplicationState(resetWorldGeneration: false);
                _diagnostics.Log("WorldSync", "Host WorldContext was invalidated.");
                return;
            }
            if (hostContext.SessionId != client.SessionId)
                throw new ProtocolException("Host WorldContext session is stale.");
            var replacingWorldContext = false;
            lock (_stateGate)
            {
                if (_worldSessionGeneration != 0
                    && hostContext.WorldGeneration < _worldSessionGeneration)
                    throw new ProtocolException("Host WorldContext generation is stale.");
                replacingWorldContext = _localWorldContext is { Ready: true } existing
                    && !SameWorldContext(existing, hostContext);
            }
            if (replacingWorldContext)
                await CloseNativeWorldBusAsync("host-world-context-changing")
                    .ConfigureAwait(false);
            Task<PreparedWorldManifest?>? automaticPreparation;
            lock (_stateGate) automaticPreparation = _automaticWorldManifestPreparation;
            var automaticPrepared = automaticPreparation is null
                ? null
                : await automaticPreparation.ConfigureAwait(false);
            var automaticMatchesHost = automaticPrepared is not null
                && automaticPrepared.Identity == hostContext.Manifest
                && string.Equals(automaticPrepared.Sha256, hostContext.ManifestSha256,
                    StringComparison.OrdinalIgnoreCase);
            if (automaticPrepared is not null && !automaticMatchesHost)
            {
                _diagnostics.Log("WorldSync",
                    $"Automatic manifest mismatch: localMap={automaticPrepared.Identity.MapContentId} " +
                    $"hostMap={hostContext.Manifest.MapContentId} " +
                    $"localCompatSha256={automaticPrepared.Sha256} " +
                    $"hostCompatSha256={hostContext.ManifestSha256} " +
                    $"localBinarySha256={automaticPrepared.BinarySha256} " +
                    $"localPakSha256={automaticPrepared.Manifest.Source.CommandsPakSha256} " +
                    $"localBinSha256={automaticPrepared.Manifest.Source.CommandsBinSha256}.");
            }
            var prepared = automaticMatchesHost
                ? automaticPrepared!
                : WorldManifestSessionLoader.LoadFromCache(hostContext);
            var load = await _controller.ExecuteCommandAsync(
                GameCommandNames.WorldManifestLoad,
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["manifestPath"] = prepared.Path
                },
                _lifetime.Token).ConfigureAwait(false);
            if (!load.Executed)
                throw new ProtocolException(
                    $"Local WORLD_MANIFEST_LOAD failed: {load.StatusCode} - {load.Message}");
            var local = hostContext with { PeerId = client.PeerId, Ready = true };
            lock (_stateGate)
            {
                _localWorldContext = local;
                _worldSessionGeneration = local.WorldGeneration;
                _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
                if (_worldResumeCheckpoint is { } resume
                    && resume.SessionId == local.SessionId
                    && resume.WorldGeneration == local.WorldGeneration
                    && resume.Manifest == local.Manifest)
                {
                    _lastCommittedWorldSequence = resume.LastCommittedSequence;
                    _worldRecoveryRequired = resume.RecoveryRequired;
                    foreach (var cause in resume.PendingAwaitAuthorityCauses)
                        _pendingAwaitAuthorityCauses.Add(cause);
                }
                else
                {
                    _worldResumeCheckpoint = null;
                    _lastCommittedWorldSequence = 0;
                    _worldRecoveryRequired = false;
                    _pendingAwaitAuthorityCauses.Clear();
                }
            }
            await client.SendWorldContextAsync(local, _lifetime.Token).ConfigureAwait(false);
            _diagnostics.Log("WorldSync",
                $"WorldContext compatible: map={local.Manifest.MapContentId} compatSha256={local.ManifestSha256}.");
            await RequestWorldRecoveryAsync(client, local, "context-ready/reconnect")
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"WorldContext compatibility failed: {exception.Message}");
        }
    }

    private void InvalidateWorldContextForRuntimeGeneration(int generation)
    {
        WorldNetworkContext? old = null;
        LobbyHost? host = null;
        LobbyClient? client = null;
        lock (_stateGate)
        {
            if (_localWorldContext is null || _localWorldRuntimeGeneration == generation) return;
            old = _localWorldContext;
            host = _host;
            client = _client;
            ResetWorldReplicationState(resetWorldGeneration: false);
        }
        _diagnostics.Log("WorldSync",
            $"Local WorldContext invalidated by LevelGeneration change to {generation}.");
        _ = CloseNativeWorldBusAsync("runtime-generation-changed");
        if (old is null) return;
        var invalid = old with { PeerId = host?.HostPeerId ?? client?.PeerId ?? old.PeerId, Ready = false };
        if (host is not null) _ = SetHostWorldContextBestEffortAsync(host, invalid);
        else if (client is not null) _ = SendClientWorldContextBestEffortAsync(client, invalid);
    }

    private async Task SetHostWorldContextBestEffortAsync(LobbyHost host, WorldNetworkContext context)
    {
        try { await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false); }
        catch (Exception exception) when (exception is ProtocolException or IOException or OperationCanceledException)
        { if (!_lifetime.IsCancellationRequested) _diagnostics.Log("WorldSync", $"WorldContext invalidation send failed: {exception.Message}"); }
    }

    private async Task SendClientWorldContextBestEffortAsync(LobbyClient client, WorldNetworkContext context)
    {
        try { await client.SendWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false); }
        catch (Exception exception) when (exception is ProtocolException or IOException or OperationCanceledException)
        { if (!_lifetime.IsCancellationRequested) _diagnostics.Log("WorldSync", $"WorldContext invalidation send failed: {exception.Message}"); }
    }

    private void ResetWorldReplicationState(
        bool resetWorldGeneration = true,
        bool preserveCommittedCheckpoint = false)
    {
        if (preserveCommittedCheckpoint && _localWorldContext is { Ready: true } current)
        {
            _worldResumeCheckpoint = new WorldResumeCheckpoint(
                current.SessionId,
                current.WorldGeneration,
                current.Manifest,
                _lastCommittedWorldSequence,
                _worldRecoveryRequired,
                _pendingAwaitAuthorityCauses.ToArray());
        }
        else if (!preserveCommittedCheckpoint)
        {
            _worldResumeCheckpoint = null;
        }
        _activeWorldTransaction?.Completion.TrySetCanceled();
        _activeWorldTransaction = null;
        // Exact NativeDispatchId correlation makes old sequence tombstones
        // unnecessary. Late callbacks from cleared dispatches are unknown/stale
        // and cannot be rebound to any future transaction.
        _worldResultDispatches.Clear();
        _currentHostAuthorityContext = null;
        _worldRecoveryAuthorityContext = null;
        _pendingAwaitAuthorityCauses.Clear();
        _lastCommittedWorldSequence = 0;
        _worldRecoveryTargetSequence = 0;
        _worldRecoveryAwaitSnapshot.Clear();
        _worldRecoveryRequired = false;
        _worldRecoveryRequestRunning = false;
        _localWorldContext = null;
        if (resetWorldGeneration) _worldSessionGeneration = 0;
        _localWorldRuntimeGeneration = 0;
        _automaticWorldManifestPreparation = null;
        _remoteEquipmentCatalogPreparation = null;
        _worldActivationRunning = false;
        _worldActivationAttempted = false;
        ClearPendingWorldProbeState();
    }

    private void ClearPendingWorldProbeState()
    {
        lock (_stateGate)
        {
            _pendingWorldProbeSequence = 0;
            _pendingWorldProbeResult?.TrySetCanceled();
            _pendingWorldProbeAckResult?.TrySetCanceled();
            _pendingWorldProbeResult = null;
            _pendingWorldProbeAckResult = null;
            _pendingWorldProbeExpectedPeers = null;
            _pendingWorldProbeAcks.Clear();
        }
    }

    private PlayerSpawnPositionPayload GetLocalSpawnPosition(ulong peerId)
    {
        lock (_stateGate)
        {
            if (!_positionAvailable || !_localGameplayReady)
                throw new InvalidOperationException("The host player is not ready in gameplay.");
            return RemoteSpawnPlanner.CreateNetworkPosition(peerId, _position);
        }
    }

    private (LobbyHost Host, int Generation) GetSpawnCommandTarget()
    {
        lock (_stateGate)
        {
            if (_host is null) throw new InvalidOperationException(
                "Only the lobby host can execute the connected-player spawn command.");
            if (_start is null) throw new InvalidOperationException(
                "Start the multiplayer session before spawning connected players.");
            if ((_lobby?.Players.Count ?? 0) < 2) throw new InvalidOperationException(
                "At least one remote player must be connected.");
            if (!_localGameplayReady || !_positionAvailable) throw new InvalidOperationException(
                "Wait until the host player is fully loaded before spawning remote players.");
            if (_spawnCommandCollecting || _pendingSpawnRequest is not null || _spawnRequestRunning)
                throw new InvalidOperationException("A remote-player spawn command is already active.");
            _spawnCommandCollecting = true;
            return (_host, _start.LevelGeneration);
        }
    }

    private static GameCommandResultPayload CreateSpawnBroadcastResult(
        SpawnRemotePlayersPayload payload) => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        payload.RequestId,
        payload.LevelGeneration,
        Guid.Empty,
        LauncherGameCommandNames.SpawnConnectedPlayers,
        "REMOTE_PLAYER_SPAWN_BROADCAST",
        "The spawn command was broadcast to every connected launcher.",
        true);

    private Task LogCharacterSelectionAsync(ScaleformBridgeMessage message)
    {
        var character = message.Get("characterName", message.Get("characterId", "unknown"));
        _diagnostics.Log("Multiplayer", $"Local character selected: {character}.");
        return Task.CompletedTask;
    }

    private async Task SendEmptyServerListAsync()
    {
        await _bridge.SendAsync("SERVER_LIST", new Dictionary<string, string>
        { ["count"] = "0" }).ConfigureAwait(false);
        await SendStatusAsync("Server discovery is disabled; use Direct Connection.")
            .ConfigureAwait(false);
    }

    private void AttachHost(LobbyHost host)
    {
        host.SnapshotChanged += OnLobbySnapshot;
        host.Started += OnLobbyStarted;
        host.GameplayReadyReceived += OnGameplayReady;
        host.SpawnRemotePlayersRequested += OnSpawnRemotePlayersRequested;
        host.PlayerPositionReceived += OnPlayerPositionReceived;
        host.PlayerEquipmentReceived += OnPlayerEquipmentReceived;
        host.WorldContextReceived += OnWorldContextReceived;
        host.WorldMutationAccepted += OnWorldMutationAccepted;
        host.WorldIntentRejected += OnWorldIntentRejected;
        host.WorldMutationAckReceived += OnWorldMutationAckReceived;
        host.Log += OnNetworkLog;
    }

    private void DetachHost(LobbyHost host)
    {
        host.SnapshotChanged -= OnLobbySnapshot;
        host.Started -= OnLobbyStarted;
        host.GameplayReadyReceived -= OnGameplayReady;
        host.SpawnRemotePlayersRequested -= OnSpawnRemotePlayersRequested;
        host.PlayerPositionReceived -= OnPlayerPositionReceived;
        host.PlayerEquipmentReceived -= OnPlayerEquipmentReceived;
        host.WorldContextReceived -= OnWorldContextReceived;
        host.WorldMutationAccepted -= OnWorldMutationAccepted;
        host.WorldIntentRejected -= OnWorldIntentRejected;
        host.WorldMutationAckReceived -= OnWorldMutationAckReceived;
        host.Log -= OnNetworkLog;
    }

    private void AttachClient(LobbyClient client)
    {
        client.SnapshotChanged += OnLobbySnapshot;
        client.Started += OnLobbyStarted;
        client.GameplayReadyReceived += OnGameplayReady;
        client.SpawnPositionRequested += OnSpawnPositionRequested;
        client.SpawnRemotePlayersRequested += OnSpawnRemotePlayersRequested;
        client.PlayerPositionReceived += OnPlayerPositionReceived;
        client.PlayerEquipmentReceived += OnPlayerEquipmentReceived;
        client.WorldContextReceived += OnWorldContextReceived;
        client.WorldMutationReceived += OnWorldMutationReceived;
        client.WorldIntentRejected += OnWorldIntentRejected;
        client.WorldRecoveryStatusReceived += OnWorldRecoveryStatusReceived;
        client.ErrorReceived += OnNetworkError;
        client.Disconnected += OnClientDisconnected;
        client.Log += OnNetworkLog;
    }

    private void DetachClient(LobbyClient client)
    {
        client.SnapshotChanged -= OnLobbySnapshot;
        client.Started -= OnLobbyStarted;
        client.GameplayReadyReceived -= OnGameplayReady;
        client.SpawnPositionRequested -= OnSpawnPositionRequested;
        client.SpawnRemotePlayersRequested -= OnSpawnRemotePlayersRequested;
        client.PlayerPositionReceived -= OnPlayerPositionReceived;
        client.PlayerEquipmentReceived -= OnPlayerEquipmentReceived;
        client.WorldContextReceived -= OnWorldContextReceived;
        client.WorldMutationReceived -= OnWorldMutationReceived;
        client.WorldIntentRejected -= OnWorldIntentRejected;
        client.WorldRecoveryStatusReceived -= OnWorldRecoveryStatusReceived;
        client.ErrorReceived -= OnNetworkError;
        client.Disconnected -= OnClientDisconnected;
        client.Log -= OnNetworkLog;
    }

    private void OnLobbySnapshot(object? sender, LobbySnapshotPayload value) =>
        _ = PublishLobbyAsync(value);

    private async Task PublishLobbyAsync(LobbySnapshotPayload? value)
    {
        if (value is null) return;
        var departedCharacters = UpdateLobbySnapshot(value);
        if (departedCharacters.Length != 0)
            await StopRemoteCharactersBestEffortAsync(departedCharacters).ConfigureAwait(false);
        PublishState();
        await SendLobbyStateAsync(value).ConfigureAwait(false);
    }

    private uint[] UpdateLobbySnapshot(LobbySnapshotPayload value)
    {
        lock (_stateGate)
        {
            _lobby = value;
            var departed = RemoveDepartedRemotePlayers(value);
            var activePeers = value.Players.Select(player => player.PeerId).ToHashSet();
            _remoteGameplayReadyPeers.RemoveWhere(peerId => !activePeers.Contains(peerId));
            _remoteGameplayReady = AreAllRemoteGameplayPlayersReady();
            return departed;
        }
    }

    private uint[] RemoveDepartedRemotePlayers(LobbySnapshotPayload value)
    {
        var activePeers = value.Players.Select(player => player.PeerId).ToHashSet();
        var departedPeers = _remoteCharacters.Keys.Where(peerId => !activePeers.Contains(peerId)).ToArray();
        var characters = departedPeers.Select(peerId => _remoteCharacters[peerId]).Distinct().ToArray();
        foreach (var peerId in departedPeers) RemoveRemotePeer(peerId);
        return characters;
    }

    private void RemoveRemotePeer(ulong peerId)
    {
        _remoteCharacters.Remove(peerId);
        _remotePositions.Remove(peerId);
        _remoteEquipment.Remove(peerId);
        _remoteSpawnRetryAttempts.Remove(peerId);
        _remoteSpawnRetryNotBeforeUtc.Remove(peerId);
    }

    private void OnLobbyStarted(object? sender, LobbyStartPayload value) =>
        _ = HandleLobbyStartedAsync(value);

    private async Task HandleLobbyStartedAsync(LobbyStartPayload value)
    {
        var worldSyncEnabled = _settingsStore.Load().WorldSyncEnabled;
        uint[] previousCharacters;
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            previousCharacters = _remoteCharacters.Values.Distinct().ToArray();
            ResetGameplayState(value);
            _worldSyncEnabledForSession = worldSyncEnabled;
            host = _host;
            client = _client;
        }
        if (host is not null)
            await SetNativeWorldSyncLifecycleAsync(
                host.SessionId, host.HostPeerId, true, value.LevelGeneration,
                true, false, "lobby-start")
                .ConfigureAwait(false);
        else if (client is not null)
            await SetNativeWorldSyncLifecycleAsync(
                client.SessionId, client.PeerId, false, value.LevelGeneration,
                true, false, "lobby-start")
                .ConfigureAwait(false);
        BeginRemoteEquipmentCatalogPreparation(value);
        if (worldSyncEnabled)
        {
            BeginAutomaticWorldManifestPreparation(value);
        }
        else
        {
            _diagnostics.Log("WorldSync",
                "WorldSync is disabled in Settings for this game session. Character spawn and movement replication remain active; no world manifest will be compiled or loaded.");
        }
        await StopRemoteCharactersBestEffortAsync(previousCharacters).ConfigureAwait(false);
        PublishState();
        await PublishLobbyStartAsync(value).ConfigureAwait(false);
    }

    private async Task PublishLobbyStartAsync(LobbyStartPayload value)
    {
        _diagnostics.Log("Multiplayer", $"Session starting: {value.Level}.");
        await SendStartGameAsync(value).ConfigureAwait(false);
    }

    private void ResetGameplayState(LobbyStartPayload start)
    {
        _start = start;
        _uiGameplayLoaded = false;
        _localGameplayReady = false;
        _localReadySending = false;
        _remoteGameplayReady = false;
        _worldSyncEnabledForSession = true;
        ResetWorldReplicationState();
        ResetRemoteSpawnState();
        _gameplayReadiness.Reset();
    }

    private void ResetRemoteSpawnState()
    {
        _pendingSpawnPositionRequest = null;
        _spawnPositionSending = false;
        _spawnCommandCollecting = false;
        _pendingSpawnRequest = null;
        _spawnRequestRunning = false;
        _remoteSpawnConvergenceRunning = false;
        _remoteSpawnRetryAttempts.Clear();
        _remoteSpawnRetryNotBeforeUtc.Clear();
        _remoteGameplayReadyPeers.Clear();
        _remoteCharacters.Clear();
        _remotePositions.Clear();
        _processedSpawnRequests.Clear();
        _pendingLocalPosition = null;
        _positionSendRunning = false;
        _localPositionSequence = 0;
        // Native PlayerSync state belongs to the AI.exe runtime, not to a lobby
        // session. Preserve the latest observed state across network restarts so
        // a new session can immediately publish a fresh snapshot.
        _lastQueuedEquipmentState = null;
        _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
        _pendingLocalEquipment = null;
        _equipmentSendRunning = false;
        _localEquipmentSequence = 0;
        _remoteEquipment.Clear();
        _movementCommandRunning = false;
    }

    private void OnGameplayReady(object? sender, GameplayReadyPayload value) =>
        _ = HandleGameplayReadyAsync(value);

    private async Task HandleGameplayReadyAsync(GameplayReadyPayload value)
    {
        if (!IsRemoteGameplayReady(value)) return;
        lock (_stateGate)
        {
            _remoteGameplayReadyPeers.Add(value.PeerId);
            _remoteGameplayReady = AreAllRemoteGameplayPlayersReady();
        }
        _diagnostics.Log("Multiplayer", $"Remote player is in gameplay (PeerId {value.PeerId}).");
        PublishState();
        var runtime = _controller.Snapshot;
        await TryAdvanceGameplayAsync(runtime).ConfigureAwait(false);
        await TryConvergeRemoteCharactersAsync(runtime, DateTimeOffset.UtcNow)
            .ConfigureAwait(false);
    }

    private bool IsRemoteGameplayReady(GameplayReadyPayload value)
    {
        lock (_stateGate)
        {
            var localPeer = _host?.HostPeerId ?? _client?.PeerId ?? 0;
            return _start is not null && value.SessionId == _start.SessionId
                && value.LevelGeneration == _start.LevelGeneration && value.PeerId != localPeer;
        }
    }

    private bool AreAllRemoteGameplayPlayersReady()
        => RemoteSpawnPlanner.AllRemotePlayersReady(
            _lobby,
            _host?.HostPeerId ?? _client?.PeerId ?? 0,
            _remoteGameplayReadyPeers);

    private void OnPlayerPositionReceived(
        object? sender,
        PlayerPositionSnapshotPayload value)
    {
        var now = DateTimeOffset.UtcNow;
        if (!AcceptRemotePosition(value, now)) return;
        var runtime = _controller.Snapshot;
        _ = TryConvergeRemoteCharactersAsync(runtime, now);
        _ = TryAdvanceRemoteMovementAsync(runtime, now);
    }

    private bool AcceptRemotePosition(
        PlayerPositionSnapshotPayload value,
        DateTimeOffset receivedAtUtc)
    {
        lock (_stateGate)
        {
            if (_start is null || _lobby is null || value.PeerId == LocalPeerId()) return false;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return false;
            if (_lobby.Players.All(player => player.PeerId != value.PeerId)) return false;
            if (!_remotePositions.TryGetValue(value.PeerId, out var track))
                _remotePositions[value.PeerId] = track = new RemotePositionTrack();
            return track.TryAccept(value, receivedAtUtc);
        }
    }

    private void OnNativePlayerSyncStateProduced(
        object? sender,
        NativePlayerEquipmentStatePayload value)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        lock (_stateGate)
        {
            _localEquipmentState = value.Valid ? value : null;
            if (!value.Valid)
            {
                _lastQueuedEquipmentState = null;
                _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
            }
        }

        if (value.Valid && TryQueueLocalEquipment(DateTimeOffset.UtcNow))
            _ = PumpLocalEquipmentAsync();
    }

    private void OnPlayerEquipmentReceived(
        object? sender,
        PlayerEquipmentSnapshotPayload value)
    {
        PlayerEquipmentSnapshotPayload? previous;
        uint characterBase;
        lock (_stateGate)
        {
            if (_start is null || _lobby is null || value.PeerId == LocalPeerId()) return;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return;
            if (_lobby.Players.All(player => player.PeerId != value.PeerId)) return;
            if (_remoteEquipment.TryGetValue(value.PeerId, out previous)
                && value.Sequence <= previous.Sequence) return;
            _remoteEquipment[value.PeerId] = value;
            _remoteCharacters.TryGetValue(value.PeerId, out characterBase);
        }

        var changed = previous is null
            || previous.CurrentEquipmentIndex != value.CurrentEquipmentIndex
            || previous.PreviousEquipmentIndex != value.PreviousEquipmentIndex
            || previous.EquipmentActive != value.EquipmentActive
            || previous.PresentationState != value.PresentationState;
        if (!changed) return;

        var characterText = characterBase == 0
            ? "CharacterBase=pending"
            : $"CharacterBase=0x{characterBase:X8}";
        _diagnostics.Log("PlayerSync",
            $"Remote PeerId {value.PeerId}: equipment={value.CurrentEquipmentIndex}, previous={value.PreviousEquipmentIndex}, " +
            $"active={(value.EquipmentActive ? 1 : 0)}, state={value.PresentationState}, {characterText}.");
    }

    private void OnSpawnPositionRequested(
        object? sender,
        SpawnPositionRequestPayload value) =>
        _ = QueueSpawnPositionRequestAsync(value);

    private async Task QueueSpawnPositionRequestAsync(SpawnPositionRequestPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return;
            _pendingSpawnPositionRequest = value;
        }
        _diagnostics.Log("Multiplayer",
            $"Host requested the local spawn position ({value.RequestId:N}).");
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private void OnSpawnRemotePlayersRequested(
        object? sender,
        SpawnRemotePlayersPayload value) =>
        _ = QueueSpawnRemotePlayersAsync(value);

    private async Task QueueSpawnRemotePlayersAsync(SpawnRemotePlayersPayload value)
    {
        if (!AcceptSpawnRequest(value)) return;
        _diagnostics.Log("Multiplayer",
            $"Remote-player spawn request {value.RequestId:N} received from the host.");
        PublishState();
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private bool AcceptSpawnRequest(SpawnRemotePlayersPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _lobby is null) return false;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return false;
            var host = _lobby.Players.SingleOrDefault(player => player.IsHost);
            if (host?.PeerId != value.HostPeerId) return false;
            if (!RemoteSpawnPlanner.MatchesLobby(value.Players, _lobby.Players)) return false;
            if (_processedSpawnRequests.Contains(value.RequestId)) return false;
            if (_pendingSpawnRequest is not null) return false;
            _pendingSpawnRequest = value;
            return true;
        }
    }

    private bool TryQueueLocalPosition(DateTimeOffset now)
    {
        lock (_stateGate)
        {
            if (_start is null || !_localGameplayReady || !_positionAvailable)
                return false;
            var peerId = LocalPeerId();
            if (peerId == 0 || (_host is null && _client is null)) return false;
            _pendingLocalPosition = new PlayerPositionSnapshotPayload(_start.SessionId, peerId,
                _start.LevelGeneration, ++_localPositionSequence, now.ToUnixTimeMilliseconds(),
                _position.X, _position.Y, _position.Z, _yaw, _moveType, _nativeMoveSpeed);
            if (_positionSendRunning) return false;
            _positionSendRunning = true;
            return true;
        }
    }

    private async Task PumpLocalPositionsAsync()
    {
        try
        {
            while (TakePendingLocalPosition() is { } value)
                await SendLocalPositionAsync(value).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("Network", $"Position publishing stopped: {exception.Message}");
            lock (_stateGate) { _pendingLocalPosition = null; _positionSendRunning = false; }
        }
    }

    private PlayerPositionSnapshotPayload? TakePendingLocalPosition()
    {
        lock (_stateGate)
        {
            var value = _pendingLocalPosition;
            _pendingLocalPosition = null;
            if (value is null) _positionSendRunning = false;
            return value;
        }
    }

    private Task SendLocalPositionAsync(PlayerPositionSnapshotPayload value)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null) return host.PublishHostPlayerPositionAsync(value, _lifetime.Token);
        if (client is not null) return client.SendPlayerPositionAsync(value, _lifetime.Token);
        throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private bool TryQueueLocalEquipment(DateTimeOffset now)
    {
        lock (_stateGate)
        {
            if (_start is null || !_localGameplayReady
                || _localEquipmentState is not { Valid: true } state) return false;
            var peerId = LocalPeerId();
            if (peerId == 0 || (_host is null && _client is null)) return false;

            var changed = _lastQueuedEquipmentState is null
                || _lastQueuedEquipmentState.CurrentEquipmentIndex != state.CurrentEquipmentIndex
                || _lastQueuedEquipmentState.PreviousEquipmentIndex != state.PreviousEquipmentIndex
                || _lastQueuedEquipmentState.EquipmentActive != state.EquipmentActive
                || _lastQueuedEquipmentState.PresentationState != state.PresentationState;
            if (!changed && now < _nextEquipmentRefreshAtUtc) return false;

            _lastQueuedEquipmentState = state;
            _nextEquipmentRefreshAtUtc = now.AddSeconds(1);
            _pendingLocalEquipment = new PlayerEquipmentSnapshotPayload(
                _start.SessionId, peerId, _start.LevelGeneration, ++_localEquipmentSequence,
                now.ToUnixTimeMilliseconds(), state.CurrentEquipmentIndex,
                state.PreviousEquipmentIndex, state.EquipmentActive, state.PresentationState);

            if (changed)
            {
                _diagnostics.Log("PlayerSync",
                    $"Native local equipment changed: revision={state.Revision}, " +
                    $"current={state.CurrentEquipmentIndex}, previous={state.PreviousEquipmentIndex}, " +
                    $"active={(state.EquipmentActive ? 1 : 0)}, state={state.PresentationState}.");
            }

            if (_equipmentSendRunning) return false;
            _equipmentSendRunning = true;
            return true;
        }
    }

    private async Task PumpLocalEquipmentAsync()
    {
        try
        {
            while (TakePendingLocalEquipment() is { } value)
                await SendLocalEquipmentAsync(value).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerSync", $"Equipment publishing stopped: {exception.Message}");
            lock (_stateGate)
            {
                _pendingLocalEquipment = null;
                _equipmentSendRunning = false;
            }
        }
    }

    private PlayerEquipmentSnapshotPayload? TakePendingLocalEquipment()
    {
        lock (_stateGate)
        {
            var value = _pendingLocalEquipment;
            _pendingLocalEquipment = null;
            if (value is null) _equipmentSendRunning = false;
            return value;
        }
    }

    private Task SendLocalEquipmentAsync(PlayerEquipmentSnapshotPayload value)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            host = _host;
            client = _client;
        }
        if (host is not null)
            return host.PublishHostPlayerEquipmentAsync(value, _lifetime.Token);
        if (client is not null)
            return client.SendPlayerEquipmentAsync(value, _lifetime.Token);
        throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private async Task TryAdvanceRemoteMovementAsync(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now)
    {
        if (!TryBeginRemoteMovement(runtime, out var targets)) return;
        try
        {
            foreach (var target in targets)
                if (!await ExecuteRemoteMovementBestEffortAsync(target, now).ConfigureAwait(false)) break;
        }
        finally { lock (_stateGate) _movementCommandRunning = false; }
    }

    private async Task<bool> ExecuteRemoteMovementBestEffortAsync(
        RemoteMovementTarget target,
        DateTimeOffset now)
    {
        try
        {
            await ExecuteRemoteMovementAsync(target, now).ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { return false; }
        catch (Exception exception)
        {
            LogRemoteMovementFailure(target, exception.Message);
            return true;
        }
    }


    private void LogRemoteMovementFailure(RemoteMovementTarget target, string message)
    {
        var now = DateTimeOffset.UtcNow;
        var key = $"{target.PeerId}:{target.CharacterBase:X8}:{message}";
        lock (_stateGate)
        {
            if (key == _lastMovementFailure && now - _lastMovementFailureLoggedAt < TimeSpan.FromSeconds(2))
                return;
            _lastMovementFailure = key;
            _lastMovementFailureLoggedAt = now;
        }
        _diagnostics.Log("Replication",
            $"Remote movement update failed for PeerId {target.PeerId} / Character 0x{target.CharacterBase:X8}: {message}");
    }

    private bool TryBeginRemoteMovement(
        NativeRuntimeSnapshot runtime,
        out IReadOnlyList<RemoteMovementTarget> targets)
    {
        lock (_stateGate)
        {
            targets = Array.Empty<RemoteMovementTarget>();
            if (_movementCommandRunning || _start is null || !_localGameplayReady) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;

            var selected = SelectMovementTargets();
            if (selected.Count == 0) return false;
            _movementCommandRunning = true;
            targets = selected;
            return true;
        }
    }

    private IReadOnlyList<RemoteMovementTarget> SelectMovementTargets()
    {
        var targets = new List<RemoteMovementTarget>(_remoteCharacters.Count);
        var generation = _start?.LevelGeneration ?? 0;
        foreach (var pair in _remoteCharacters.OrderBy(value => value.Key))
        {
            if (!_remotePositions.TryGetValue(pair.Key, out var track)) continue;
            var snapshot = track.Snapshot();
            if (snapshot is not null)
                targets.Add(new RemoteMovementTarget(
                    pair.Key, pair.Value, generation, snapshot));
        }
        return targets;
    }

    private async Task ExecuteRemoteMovementAsync(
        RemoteMovementTarget target,
        DateTimeOffset now)
    {
        if (!IsMovementTargetCurrent(target)) return;
        var plan = RemoteMovementPlanner.Create(target.Track, now);
        if (plan.Action == RemoteMovementAction.None || !IsMovementTargetCurrent(target)) return;
        var arguments = RemoteMovementCommandFactory.Create(target.CharacterBase, plan);
        var result = await _controller.ExecuteReplicationCommandAsync(
            GameCommandNames.CharacterDirectMove, arguments, _lifetime.Token)
            .ConfigureAwait(false);
        EnsureMovementAccepted(result);
        CompleteMovementCommand(target, plan, DateTimeOffset.UtcNow);
    }

    private bool IsMovementTargetCurrent(RemoteMovementTarget target)
    {
        lock (_stateGate)
        {
            return _start?.LevelGeneration == target.LevelGeneration
                && _remoteCharacters.TryGetValue(target.PeerId, out var characterBase)
                && characterBase == target.CharacterBase;
        }
    }

    private static void EnsureMovementAccepted(GameCommandResultPayload result)
    {
        if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
        throw new InvalidOperationException(
            $"Character Direct Move was rejected: {result.StatusCode} - {result.Message}");
    }

    private void CompleteMovementCommand(
        RemoteMovementTarget target,
        RemoteMovementPlan plan,
        DateTimeOffset appliedAtUtc)
    {
        var moving = plan.Action == RemoteMovementAction.Move;
        var previousMoving = target.Track.CommandMoving;
        var applied = false;
        lock (_stateGate)
        {
            if (_start?.LevelGeneration == target.LevelGeneration
                && _remoteCharacters.TryGetValue(target.PeerId, out var characterBase)
                && characterBase == target.CharacterBase
                && _remotePositions.TryGetValue(target.PeerId, out var track))
            {
                track.MarkCommand(plan, appliedAtUtc);
                applied = true;
            }
        }
        if (applied && moving != previousMoving) LogMovementTransition(target, plan, moving);
    }

    private void LogMovementTransition(
        RemoteMovementTarget target,
        RemoteMovementPlan plan,
        bool moving) => _diagnostics.Log("Replication", moving
            ? $"Remote Character 0x{target.CharacterBase:X8} started replicating PeerId {target.PeerId}; authoritativePosition=({plan.PositionX:0.000},{plan.PositionY:0.000},{plan.PositionZ:0.000}), sourceStep={plan.RemoteStepDistance:0.000}, moveType={plan.MoveType}, nativeMoveSpeed={plan.NativeMoveSpeed:0.000}. The engine thread drives the native locomotion cycle; no GoTo destination is created."
            : $"Remote Character 0x{target.CharacterBase:X8} stopped for PeerId {target.PeerId}.");

    private void OnNetworkError(object? sender, ErrorPayload value) =>
        _ = SendErrorAsync($"{value.Code}: {value.Message}");

    private void OnClientDisconnected(object? sender, EventArgs value) =>
        _ = HandleClientDisconnectedAsync();

    private async Task HandleClientDisconnectedAsync()
    {
        await Task.Yield();
        await StopNetworkAsync("Disconnected from host").ConfigureAwait(false);
    }

    private void OnNetworkLog(object? sender, string value) =>
        _diagnostics.Log("Network", value);

    private void BeginRemoteEquipmentCatalogPreparation(LobbyStartPayload start)
    {
        var settings = _settingsStore.Load();
        var level = start.Level;
        var runtimeExecutable = _controller.Snapshot.ExecutablePath;
        var runtimeGameDirectory = ResolveRuntimeGameDirectory(runtimeExecutable);
        var gameDirectory = runtimeGameDirectory ?? settings.GameDirectory;
        var task = Task.Run<RemoteEquipmentCatalogSet?>(() =>
        {
            try
            {
                var source = WorldManifestAutoResolver.ResolveLevelSource(
                    gameDirectory, level);
                var catalogs = RemoteEquipmentCatalogCompiler.CompileAll(
                    source, out var diagnostic);
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment catalog: level={level} {diagnostic}");
                return catalogs;
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                if (!_lifetime.IsCancellationRequested)
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment catalog unavailable for '{level}': " +
                        $"{exception.GetType().Name}: {exception.Message}");
                return null;
            }
        }, _lifetime.Token);
        lock (_stateGate)
        {
            if (_start?.Level == level)
                _remoteEquipmentCatalogPreparation = task;
        }
    }

    private void BeginAutomaticWorldManifestPreparation(LobbyStartPayload start)
    {
        lock (_stateGate)
        {
            if (!_worldSyncEnabledForSession || _start?.Level != start.Level) return;
        }
        var settings = _settingsStore.Load();
        var level = start.Level;
        var runtimeExecutable = _controller.Snapshot.ExecutablePath;
        var runtimeGameDirectory = ResolveRuntimeGameDirectory(runtimeExecutable);
        var manifestGameDirectory = runtimeGameDirectory ?? settings.GameDirectory;
        _diagnostics.Log("WorldSync",
            $"Automatic manifest preparation started: level={level} gameRoot={manifestGameDirectory}.");
        var task = Task.Run<PreparedWorldManifest?>(() =>
        {
            try
            {
                var source = WorldManifestAutoResolver.ResolveLevelSource(
                    manifestGameDirectory, level);
                var compiled = new WorldManifestCompiler().Compile(
                    source, useCache: true);
                var prepared = WorldManifestSessionLoader.Load(compiled.BinaryPath);
                _diagnostics.Log("WorldSync",
                    $"Automatic manifest source: level={level} gameRoot={manifestGameDirectory} " +
                    $"source={source} pakPresent={(prepared.Manifest.Source.CommandsPakPresent ? 1 : 0)} " +
                    $"pakSha256={prepared.Manifest.Source.CommandsPakSha256} " +
                    $"binPresent={(prepared.Manifest.Source.CommandsBinPresent ? 1 : 0)} " +
                    $"binSha256={prepared.Manifest.Source.CommandsBinSha256}.");
                _diagnostics.Log("WorldSync",
                    $"Automatic manifest ready: level={level} map={prepared.Identity.MapContentId} " +
                    $"compiler=0x{prepared.Identity.CompilerVersion:X8} " +
                    $"compatSha256={prepared.Sha256} binarySha256={prepared.BinarySha256} " +
                    $"cacheHit={(compiled.CacheHit ? 1 : 0)}.");
                return prepared;
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                if (!_lifetime.IsCancellationRequested)
                    _diagnostics.Log("WorldSync",
                        $"Automatic manifest preparation unavailable for '{level}': " +
                        $"{exception.GetType().Name}: {exception.Message}");
                return null;
            }
        }, _lifetime.Token);
        lock (_stateGate)
        {
            if (_start?.Level == level)
            {
                _automaticWorldManifestPreparation = task;
            }
        }
    }

    private static string? ResolveRuntimeGameDirectory(string executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath)) return null;
        try
        {
            var fullPath = Path.GetFullPath(executablePath);
            if (!string.Equals(Path.GetFileName(fullPath), "AI.exe",
                    StringComparison.OrdinalIgnoreCase))
                return null;
            var directory = Path.GetDirectoryName(fullPath);
            return string.IsNullOrWhiteSpace(directory) ? null : directory;
        }
        catch (ArgumentException)
        {
            return null;
        }
        catch (NotSupportedException)
        {
            return null;
        }
    }

    private async Task TryActivateAutomaticWorldSyncAsync(NativeRuntimeSnapshot runtime)
    {
        LobbyHost? host;
        Task<PreparedWorldManifest?>? preparation;
        lock (_stateGate)
        {
            host = _host;
            if (!_worldSyncEnabledForSession || host is null || _start is null || !_localGameplayReady || !_remoteGameplayReady
                || (_lobby?.Players.Count ?? 0) < 2 || _localWorldContext is { Ready: true }
                || _worldActivationRunning || _worldActivationAttempted
                || !runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration <= 0)
                return;
            preparation = _automaticWorldManifestPreparation;
            if (preparation is null) return;
            _worldActivationRunning = true;
            _worldActivationAttempted = true;
        }

        try
        {
            var prepared = await preparation.ConfigureAwait(false);
            if (prepared is null) return;
            var load = await _controller.ExecuteCommandAsync(
                GameCommandNames.WorldManifestLoad,
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["manifestPath"] = prepared.Path
                },
                _lifetime.Token).ConfigureAwait(false);
            if (!load.Executed)
                throw new InvalidOperationException(
                    $"Automatic WORLD_MANIFEST_LOAD failed: {load.StatusCode} - {load.Message}");

            WorldNetworkContext context;
            lock (_stateGate)
            {
                _worldSessionGeneration = checked(_worldSessionGeneration + 1);
                context = new WorldNetworkContext(
                    host.SessionId,
                    host.HostPeerId,
                    _worldSessionGeneration,
                    prepared.Identity,
                    prepared.Sha256,
                    true);
                _localWorldContext = context;
                _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
            }
            await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false);
            if (!await WaitForAutomaticWorldCompatibilityAsync(host, context).ConfigureAwait(false))
                return;
            var nativeOpened = await SetNativeWorldSyncLifecycleAsync(
                context.SessionId, host.HostPeerId, true, context.WorldGeneration,
                true, true, "world-context-compatible").ConfigureAwait(false);
            if (!nativeOpened)
                throw new InvalidOperationException(
                    "Compatible WorldContext was established but native authority/bus binding failed.");
            _diagnostics.Log("WorldSync",
                $"Automatic CATHODE world bus active: generation={context.WorldGeneration} " +
                $"map={context.Manifest.MapContentId}.");
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync",
                    "Automatic CATHODE world activation failed without affecting Character replication: " +
                    $"{exception.GetType().Name}: {exception.Message}");
        }
        finally
        {
            lock (_stateGate) _worldActivationRunning = false;
        }
    }

    private async Task<bool> WaitForAutomaticWorldCompatibilityAsync(
        LobbyHost host,
        WorldNetworkContext context)
    {
        var nextProgressLog = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        while (!_lifetime.IsCancellationRequested)
        {
            lock (_stateGate)
            {
                if (!ReferenceEquals(_host, host)
                    || !_worldSyncEnabledForSession
                    || _start is null
                    || _localWorldContext is not { Ready: true } current
                    || !SameWorldContext(current, context))
                    return false;
            }

            if (host.AreWorldPeersCompatible())
                return true;

            var now = DateTimeOffset.UtcNow;
            if (now >= nextProgressLog)
            {
                _diagnostics.Log("WorldSync",
                    $"Automatic CATHODE world activation still waiting for compatible peer WorldContext: " +
                    $"generation={context.WorldGeneration} map={context.Manifest.MapContentId}.");
                nextProgressLog = now + TimeSpan.FromSeconds(30);
            }

            await Task.Delay(100, _lifetime.Token).ConfigureAwait(false);
        }

        return false;
    }

    private async Task TryAdvanceGameplayAsync(NativeRuntimeSnapshot runtime)
    {
        if (ShouldSendLocalReady(runtime))
            await SendLocalGameplayReadyAsync().ConfigureAwait(false);
        await TryActivateAutomaticWorldSyncAsync(runtime).ConfigureAwait(false);
        await TrySendPendingSpawnPositionAsync(runtime).ConfigureAwait(false);
        await TryRunPendingSpawnRequestAsync(runtime).ConfigureAwait(false);
    }

    private async Task TrySendPendingSpawnPositionAsync(NativeRuntimeSnapshot runtime)
    {
        if (!TryBeginSpawnPositionResponse(runtime)) return;
        try { await SendPendingSpawnPositionAsync().ConfigureAwait(false); }
        catch { lock (_stateGate) _spawnPositionSending = false; throw; }
    }

    private bool TryBeginSpawnPositionResponse(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_pendingSpawnPositionRequest is null || _spawnPositionSending) return false;
            if (!_uiGameplayLoaded || !_positionAvailable || !_localGameplayReady) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady || _client is null) return false;
            _spawnPositionSending = true;
            return true;
        }
    }

    private async Task SendPendingSpawnPositionAsync()
    {
        var state = GetPendingSpawnPositionState();
        await state.Client.SendSpawnPositionAsync(state.Request, state.Position,
            _lifetime.Token).ConfigureAwait(false);
        lock (_stateGate) { _pendingSpawnPositionRequest = null; _spawnPositionSending = false; }
        _diagnostics.Log("Multiplayer",
            $"Local spawn position sent to host: X={state.Position.X:0.000}, " +
            $"Y={state.Position.Y:0.000}, Z={state.Position.Z:0.000}.");
    }

    private (LobbyClient Client, SpawnPositionRequestPayload Request,
        PlayerSpawnPositionPayload Position) GetPendingSpawnPositionState()
    {
        lock (_stateGate)
        {
            var client = _client ?? throw new InvalidOperationException("Lobby client is missing.");
            var request = _pendingSpawnPositionRequest
                ?? throw new InvalidOperationException("No spawn-position request is pending.");
            return (client, request, RemoteSpawnPlanner.CreateNetworkPosition(client.PeerId, _position));
        }
    }

    private bool ShouldSendLocalReady(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_start is null || _localGameplayReady || _localReadySending) return false;
            if (!_uiGameplayLoaded || !_positionAvailable) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;
            _localReadySending = true;
            return true;
        }
    }

    private async Task SendLocalGameplayReadyAsync()
    {
        try
        {
            var generation = GetStartGeneration();
            await SendReadyOverNetworkAsync(generation).ConfigureAwait(false);
            lock (_stateGate) { _localGameplayReady = true; _localReadySending = false; }
            _diagnostics.Log("Multiplayer",
                "Local player is in gameplay; readiness sent. Per-peer Character convergence is active.");
            PublishState();
            await TryConvergeRemoteCharactersAsync(_controller.Snapshot, DateTimeOffset.UtcNow)
                .ConfigureAwait(false);
        }
        catch
        {
            lock (_stateGate) _localReadySending = false;
            throw;
        }
    }

    private int GetStartGeneration()
    {
        lock (_stateGate) return _start?.LevelGeneration
            ?? throw new InvalidOperationException("No active lobby start generation.");
    }

    private async Task SendReadyOverNetworkAsync(int generation)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null)
            await host.SetHostGameplayReadyAsync(generation).ConfigureAwait(false);
        else if (client is not null)
            await client.SendGameplayReadyAsync(generation).ConfigureAwait(false);
        else throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private async Task TryConvergeRemoteCharactersAsync(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now)
    {
        RemotePlayerSpawnTarget target;
        lock (_stateGate)
        {
            if (!TryBeginRemoteCharacterConvergence(runtime, now, out target)) return;
        }

        var gateEntered = false;
        try
        {
            await _spawnGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            gateEntered = true;

            // The target may have moved while another local spawn command owned
            // the gate. Re-read its latest live snapshot rather than spawning at
            // the position that originally caused convergence to begin.
            if (!TryRefreshIndependentSpawnTarget(target.Player.PeerId,
                    DateTimeOffset.UtcNow, out target))
                return;

            await SpawnRemotePlayerAsync(target).ConfigureAwait(false);
            RegisterIndependentSpawnSuccess(target.Player.PeerId);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            RegisterIndependentSpawnFailure(target.Player, exception);
        }
        finally
        {
            lock (_stateGate) _remoteSpawnConvergenceRunning = false;
            if (gateEntered) _spawnGate.Release();
        }
    }

    private bool TryBeginRemoteCharacterConvergence(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now,
        out RemotePlayerSpawnTarget target)
    {
        target = default!;
        if (_remoteSpawnConvergenceRunning || _start is null || _lobby is null
            || !_localGameplayReady || !_uiGameplayLoaded)
            return false;
        if (!runtime.IpcConnected || !runtime.EngineThreadReady
            || !runtime.CharacterCreateHookInstalled)
            return false;

        var localPeerId = LocalPeerId();
        foreach (var player in _lobby.Players
                     .Where(player => player.PeerId != localPeerId)
                     .OrderBy(player => player.PeerId))
        {
            if (_remoteCharacters.ContainsKey(player.PeerId)) continue;
            if (!_remoteGameplayReadyPeers.Contains(player.PeerId)) continue;
            if (_remoteSpawnRetryNotBeforeUtc.TryGetValue(player.PeerId, out var retryAt)
                && now < retryAt)
                continue;
            if (!TryCreateFreshSpawnTarget(player, now, out target)) continue;
            _remoteSpawnConvergenceRunning = true;
            return true;
        }
        return false;
    }

    private bool TryRefreshIndependentSpawnTarget(
        ulong peerId,
        DateTimeOffset now,
        out RemotePlayerSpawnTarget target)
    {
        lock (_stateGate)
        {
            target = default!;
            if (_start is null || _lobby is null || !_localGameplayReady
                || _remoteCharacters.ContainsKey(peerId)
                || !_remoteGameplayReadyPeers.Contains(peerId))
                return false;
            var player = _lobby.Players.SingleOrDefault(value => value.PeerId == peerId);
            return player is not null && TryCreateFreshSpawnTarget(player, now, out target);
        }
    }

    private bool TryCreateFreshSpawnTarget(
        LobbyPlayer player,
        DateTimeOffset now,
        out RemotePlayerSpawnTarget target)
    {
        target = default!;
        if (!_remotePositions.TryGetValue(player.PeerId, out var track)) return false;
        var snapshot = track.Snapshot();
        if (snapshot is null || now - snapshot.ReceivedAtUtc > RemoteSpawnPositionFreshness)
            return false;
        var latest = snapshot.Latest;
        if (!float.IsFinite(latest.X) || !float.IsFinite(latest.Y) || !float.IsFinite(latest.Z))
            return false;
        target = new RemotePlayerSpawnTarget(
            player,
            RemoteSpawnPlanner.CreateNetworkPosition(latest));
        return true;
    }

    private void RegisterIndependentSpawnSuccess(ulong peerId)
    {
        lock (_stateGate)
        {
            _remoteSpawnRetryAttempts.Remove(peerId);
            _remoteSpawnRetryNotBeforeUtc.Remove(peerId);
        }
    }

    private void RegisterIndependentSpawnFailure(LobbyPlayer player, Exception exception)
    {
        int attempt;
        DateTimeOffset retryAt;
        lock (_stateGate)
        {
            _remoteSpawnRetryAttempts.TryGetValue(player.PeerId, out var previousAttempt);
            attempt = previousAttempt + 1;
            _remoteSpawnRetryAttempts[player.PeerId] = attempt;
            var shift = Math.Clamp(attempt - 1, 0, 4);
            var delayMilliseconds = Math.Min(500 * (1 << shift), 5000);
            retryAt = DateTimeOffset.UtcNow.AddMilliseconds(delayMilliseconds);
            _remoteSpawnRetryNotBeforeUtc[player.PeerId] = retryAt;
        }
        _diagnostics.Log("Multiplayer",
            $"Independent Character spawn for {player.DisplayName} failed on attempt {attempt}: " +
            $"{exception.Message}. Automatic retry after {retryAt:O}.");
    }

    private async Task TryRunPendingSpawnRequestAsync(NativeRuntimeSnapshot runtime)
    {
        if (!TryBeginPendingSpawnRequest(runtime)) return;
        await _spawnGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try { await ExecutePendingSpawnRequestAsync().ConfigureAwait(false); }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) { RegisterSpawnRequestFailure(exception); }
        finally
        {
            lock (_stateGate) _spawnRequestRunning = false;
            _spawnGate.Release();
        }
    }

    private bool TryBeginPendingSpawnRequest(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_pendingSpawnRequest is null || _spawnRequestRunning) return false;
            if (!_uiGameplayLoaded || !_positionAvailable) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;
            if (!runtime.CharacterCreateHookInstalled) return false;
            _spawnRequestRunning = true;
            return true;
        }
    }

    private async Task ExecutePendingSpawnRequestAsync()
    {
        var request = GetPendingSpawnRequest();
        var players = GetUnspawnedRemotePlayers();
        if (players.Count == 0)
            _diagnostics.Log("Multiplayer", "All connected remote players already have Characters.");
        foreach (var player in players)
            await SpawnRemotePlayerAsync(player).ConfigureAwait(false);
        CompleteSpawnRequest(request.RequestId);
    }

    private SpawnRemotePlayersPayload GetPendingSpawnRequest()
    {
        lock (_stateGate) return _pendingSpawnRequest
            ?? throw new InvalidOperationException("No remote-player spawn request is pending.");
    }

    private IReadOnlyList<RemotePlayerSpawnTarget> GetUnspawnedRemotePlayers()
    {
        lock (_stateGate) return RemoteSpawnPlanner.SelectPendingPlayers(
            _lobby, _pendingSpawnRequest?.Players, LocalPeerId(), _remoteCharacters);
    }

    private async Task SpawnRemotePlayerAsync(RemotePlayerSpawnTarget target)
    {
        var previousKey = _controller.Snapshot.RemoteCharacterSpawnKey;
        var arguments = RemoteSpawnPlanner.Create(target.Position);
        LogSpawnPosition(target.Player, target.Position);
        var result = await _controller.ExecuteCommandAsync(
            GameCommandNames.SpawnCharacter, arguments, _lifetime.Token).ConfigureAwait(false);
        EnsureSpawnAccepted(result);
        var expectedSpawnKey = TryParseSpawnKey(result.Message);
        var characterBase = await WaitForTrackedSpawnAsync(previousKey, expectedSpawnKey)
            .ConfigureAwait(false);
        StoreRemoteCharacter(target.Player, characterBase);
        _diagnostics.Log("PlayerSync",
            $"Remote equipment provisioning is manual for PeerId {target.Player.PeerId}. " +
            "Use the Launcher command 'Give Equipment to Remote Characters (manual)' after the Character is visibly initialized.");
        await ShowPlayerJoinedMessageAsync(target.Player.DisplayName).ConfigureAwait(false);
    }

    private async Task<GameCommandResultPayload> ProvisionRemoteEquipmentManualAsync(
        IReadOnlyDictionary<string, string> arguments)
    {
        if (!arguments.TryGetValue("equipmentId", out var equipmentId)
            || !RemoteEquipmentDefinitions.TryGet(equipmentId, out var definition))
            throw new InvalidOperationException("Select a valid equipment entry before provisioning.");
        if (!definition.SelectorEnabled)
            throw new InvalidOperationException(
                $"'{definition.Label}' is not enabled for the external WEAPON_GiveToCharacter bridge. " +
                $"{definition.DisabledReason}");

        await _remoteEquipmentProvisionGate.WaitAsync(_lifetime.Token)
            .ConfigureAwait(false);
        try
        {
            List<(LobbyPlayer Player, uint CharacterBase)> targets;
            Task<RemoteEquipmentCatalogSet?>? preparation;
            lock (_stateGate)
            {
                if (_start is null || _lobby is null)
                    throw new InvalidOperationException(
                        "Start the multiplayer session before provisioning equipment.");

                var localPeerId = LocalPeerId();
                targets = _lobby.Players
                    .Where(player => player.PeerId != localPeerId)
                    .Select(player =>
                    {
                        _remoteCharacters.TryGetValue(player.PeerId, out var characterBase);
                        return (Player: player, CharacterBase: characterBase);
                    })
                    .Where(target => target.CharacterBase != 0U)
                    .ToList();
                preparation = _remoteEquipmentCatalogPreparation;
            }

            if (targets.Count == 0)
                throw new InvalidOperationException(
                    "No mapped remote Character is available. Spawn the remote Character first and wait until it is visibly initialized.");

            var runtime = _controller.Snapshot;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady
                || runtime.LevelGeneration <= 0)
                throw new InvalidOperationException(
                    "Wait until the DLL and certified engine thread are ready before provisioning equipment.");

            if (preparation is null)
                throw new InvalidOperationException(
                    "The equipment catalog has not been prepared for the current level.");

            var catalogs = await preparation.ConfigureAwait(false);
            if (catalogs is null)
                throw new InvalidOperationException(
                    "The equipment catalog is unavailable for the current level.");
            if (!catalogs.TryGet(equipmentId, out var catalog))
            {
                catalogs.Unavailable.TryGetValue(equipmentId, out var reason);
                throw new InvalidOperationException(
                    $"'{definition.Label}' is not provisionable from this level's COMMANDS graph. {reason}");
            }

            _diagnostics.Log("PlayerSync",
                $"Manual remote equipment provisioning started: equipment='{definition.Label}', " +
                $"composite='{catalog.CompositeName}', targets={targets.Count}.");

            var succeeded = 0;
            var failed = new List<ulong>();
            foreach (var target in targets)
            {
                var provisioned = await ProvisionRemoteEquipmentAsync(
                    target.Player, target.CharacterBase, catalog).ConfigureAwait(false);
                if (provisioned)
                    ++succeeded;
                else
                    failed.Add(target.Player.PeerId);
            }

            var complete = failed.Count == 0;
            var status = complete
                ? "REMOTE_EQUIPMENT_MANUAL_COMPLETE"
                : "REMOTE_EQUIPMENT_MANUAL_PARTIAL";
            var failedText = failed.Count == 0
                ? "none"
                : string.Join(",", failed.Select(value =>
                    value.ToString(CultureInfo.InvariantCulture)));
            var message =
                $"Manual equipment provisioning finished: equipment='{definition.Label}' " +
                $"targets={targets.Count} succeeded={succeeded} failed={failed.Count} " +
                $"failedPeers={failedText}.";
            _diagnostics.Log("PlayerSync", message);

            return new GameCommandResultPayload(
                ProtocolConstants.ExperimentalSchemaVersion,
                Guid.NewGuid(),
                runtime.LevelGeneration,
                Guid.Empty,
                LauncherGameCommandNames.ProvisionRemoteEquipment,
                status,
                message,
                complete && succeeded > 0);
        }
        finally
        {
            _remoteEquipmentProvisionGate.Release();
        }
    }

    private async Task<bool> ProvisionRemoteEquipmentAsync(
        LobbyPlayer player,
        uint characterBase,
        RemoteEquipmentCatalog catalog)
    {
        var arguments = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["characterAddress"] = $"0x{characterBase:X8}",
            ["compositeName"] = catalog.CompositeName,
            ["equipmentEntityShortGuid"] =
                $"0x{catalog.EquipmentEntityShortGuid:X8}",
            ["equipmentOutputShortGuid"] =
                $"0x{catalog.EquipmentOutputShortGuid:X8}",
            ["spawnSchedulerParameterShortGuid"] =
                $"0x{catalog.SpawnSchedulerParameterShortGuid:X8}",
            ["spawnTargetInputShortGuid"] =
                $"0x{catalog.SpawnTargetInputShortGuid:X8}",
            ["equipmentSlot"] =
                catalog.EquipmentSlot.ToString(CultureInfo.InvariantCulture)
        };
        var deadline = DateTimeOffset.UtcNow.AddSeconds(8);
        string lastStatus = string.Empty;
        string lastMessage = string.Empty;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var result = await _controller.ExecuteReplicationCommandAsync(
                    GameCommandNames.RemoteEquipmentProvision,
                    arguments,
                    _lifetime.Token).ConfigureAwait(false);
                if (result.Executed)
                {
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment assigned through WEAPON_GiveToCharacter: " +
                        $"PeerId={player.PeerId} CharacterBase=0x{characterBase:X8} " +
                        $"equipment='{catalog.Definition.Label}' composite='{catalog.CompositeName}'.");
                    return true;
                }

                lastStatus = result.StatusCode;
                lastMessage = result.Message;
                if (!string.Equals(result.StatusCode,
                        "REMOTE_EQUIPMENT_NOT_READY", StringComparison.Ordinal))
                {
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment assignment failed for PeerId {player.PeerId} " +
                        $"('{catalog.Definition.Label}'): {result.StatusCode} - {result.Message}");
                    return false;
                }
            }
            catch (TimeoutException exception)
            {
                lastStatus = "COMMAND_TIMEOUT";
                lastMessage = exception.Message;
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment command timed out for PeerId {player.PeerId}; " +
                    $"retrying observation of the same provisioning operation: {exception.Message}");
            }
            catch (Exception exception) when (
                exception is ProtocolException or IOException or InvalidOperationException)
            {
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment assignment failed for PeerId {player.PeerId}: {exception.Message}");
                return false;
            }

            await Task.Delay(250, _lifetime.Token).ConfigureAwait(false);
        }

        _diagnostics.Log("PlayerSync",
            $"Remote equipment assignment timed out for PeerId {player.PeerId}: " +
            $"{lastStatus} - {lastMessage}");
        return false;
    }

    private async Task ShowPlayerJoinedMessageAsync(string playerName)
    {
        try
        {
            var arguments = RemoteSpawnPlanner.CreateJoinMessage(playerName);
            var result = await _controller.ExecuteCommandAsync(
                GameCommandNames.PopupMessage, arguments, _lifetime.Token)
                .ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
            _diagnostics.Log("Multiplayer",
                $"Player join PopMessage was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Multiplayer",
                $"Player join PopMessage failed without interrupting spawn: {exception.Message}");
        }
    }

    private static void EnsureSpawnAccepted(GameCommandResultPayload result)
    {
        if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
        throw new InvalidOperationException(
            $"SpawnCharacter was rejected: {result.StatusCode} - {result.Message}");
    }

    private async Task<uint> WaitForTrackedSpawnAsync(
        uint previousKey,
        uint? expectedSpawnKey)
    {
        var deadline = DateTimeOffset.UtcNow.AddSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var resolved = ResolveTrackedSpawn(previousKey, expectedSpawnKey, _controller.Snapshot);
            if (resolved.HasValue) return resolved.Value;
            await Task.Delay(25, _lifetime.Token).ConfigureAwait(false);
        }
        throw new TimeoutException("The Spawn Tracker did not publish CharacterBase in time.");
    }

    private static uint? ResolveTrackedSpawn(
        uint previousKey,
        uint? expectedSpawnKey,
        NativeRuntimeSnapshot runtime)
    {
        if (expectedSpawnKey.HasValue)
        {
            if (runtime.RemoteCharacterSpawnKey != expectedSpawnKey.Value) return null;
        }
        else if (runtime.RemoteCharacterSpawnKey == previousKey)
        {
            return null;
        }

        if (runtime.RemoteCharacterState == RemoteCharacterStateNames.Failed)
            throw new InvalidOperationException("The Spawn Tracker reported a failed Character spawn.");
        return runtime.RemoteCharacterState == RemoteCharacterStateNames.Ready
            ? runtime.RemoteCharacterBase
            : null;
    }

    private static uint? TryParseSpawnKey(string? message)
    {
        const string marker = "spawnKey=";
        if (string.IsNullOrEmpty(message)) return null;
        var markerIndex = message.IndexOf(marker, StringComparison.Ordinal);
        if (markerIndex < 0) return null;
        var start = markerIndex + marker.Length;
        var end = start;
        while (end < message.Length && message[end] >= '0' && message[end] <= '9') end++;
        if (end == start) return null;
        return uint.TryParse(message.AsSpan(start, end - start), NumberStyles.None,
            CultureInfo.InvariantCulture, out var key)
            ? key
            : null;
    }

    private void StoreRemoteCharacter(LobbyPlayer player, uint characterBase)
    {
        lock (_stateGate)
        {
            _remoteCharacters[player.PeerId] = characterBase;
            _remoteSpawnRetryAttempts.Remove(player.PeerId);
            _remoteSpawnRetryNotBeforeUtc.Remove(player.PeerId);
        }
        _diagnostics.Log("Multiplayer",
            $"Remote player {player.DisplayName} mapped to CharacterBase=0x{characterBase:X8}.");
        PlayerEquipmentSnapshotPayload? equipment;
        lock (_stateGate) _remoteEquipment.TryGetValue(player.PeerId, out equipment);
        if (equipment is not null)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote PeerId {player.PeerId} state is ready for CharacterBase=0x{characterBase:X8}: " +
                $"equipment={equipment.CurrentEquipmentIndex}, previous={equipment.PreviousEquipmentIndex}, active={(equipment.EquipmentActive ? 1 : 0)}, " +
                $"state={equipment.PresentationState}.");
        }
    }

    private void CompleteSpawnRequest(Guid requestId)
    {
        lock (_stateGate)
        {
            _processedSpawnRequests.Add(requestId);
            if (_pendingSpawnRequest?.RequestId == requestId) _pendingSpawnRequest = null;
        }
        _diagnostics.Log("Multiplayer", $"Remote-player spawn request {requestId:N} completed.");
        PublishState();
    }

    private void RegisterSpawnRequestFailure(Exception exception)
    {
        Guid? requestId;
        lock (_stateGate)
        {
            requestId = _pendingSpawnRequest?.RequestId;
            if (requestId.HasValue) _processedSpawnRequests.Add(requestId.Value);
            _pendingSpawnRequest = null;
        }
        _diagnostics.Log("Multiplayer",
            $"Remote-player spawn request failed: {exception.Message}");
    }

    private void LogSpawnPosition(
        LobbyPlayer player,
        PlayerSpawnPositionPayload position) =>
        _diagnostics.Log("Multiplayer",
            $"Spawning Character for {player.DisplayName} at that player's network position: " +
            $"X={position.X:0.000}, Y={position.Y:0.000}, Z={position.Z:0.000}.");

    private async Task StopNetworkAsync(string reason)
    {
        var stopState = BeginNetworkStop();
        DetachNetwork(stopState.Host, stopState.Client);
        await UnbindNativeWorldSyncAuthorityAsync($"network-stop:{reason}")
            .ConfigureAwait(false);
        await StopRemoteCharactersBestEffortAsync(stopState.RemoteCharacters).ConfigureAwait(false);
        lock (_stateGate) ClearSessionState();
        await DisposeNetworkAsync(stopState.Host, stopState.Client).ConfigureAwait(false);
        _diagnostics.Log("Multiplayer", reason);
        PublishState();
        await SendStatusAsync("Lobby disconnected").ConfigureAwait(false);
    }

    private (LobbyHost? Host, LobbyClient? Client, uint[] RemoteCharacters) BeginNetworkStop()
    {
        lock (_stateGate)
        {
            _localGameplayReady = false;
            var characters = _remoteCharacters.Values.Distinct().ToArray();
            _start = null;
            return (_host, _client, characters);
        }
    }

    private void DetachNetwork(LobbyHost? host, LobbyClient? client)
    {
        if (host is not null) DetachHost(host);
        if (client is not null) DetachClient(client);
    }

    private static async Task DisposeNetworkAsync(LobbyHost? host, LobbyClient? client)
    {
        if (host is not null) await host.DisposeAsync().ConfigureAwait(false);
        if (client is not null) await client.DisposeAsync().ConfigureAwait(false);
    }

    private async Task StopRemoteCharactersBestEffortAsync(IEnumerable<uint> characterBases)
    {
        foreach (var characterBase in characterBases.Distinct())
            await StopRemoteCharacterBestEffortAsync(characterBase).ConfigureAwait(false);
    }

    private async Task StopRemoteCharacterBestEffortAsync(uint characterBase)
    {
        try
        {
            var runtime = _controller.Snapshot;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return;
            var result = await _controller.ExecuteReplicationCommandAsync(
                GameCommandNames.CharacterDirectMove, RemoteMovementCommandFactory.CreateStop(characterBase),
                CancellationToken.None).ConfigureAwait(false);
            EnsureMovementAccepted(result);
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Replication",
                $"Best-effort stop failed for Character 0x{characterBase:X8}: {exception.Message}");
        }
    }

    private void ClearSessionState()
    {
        _host = null;
        _client = null;
        _lobby = null;
        _start = null;
        _uiGameplayLoaded = false;
        _localGameplayReady = false;
        _localReadySending = false;
        _remoteGameplayReady = false;
        ResetWorldReplicationState(preserveCommittedCheckpoint: true);
        ResetRemoteSpawnState();
        _localPositionSequence = 0;
        _gameplayReadiness.Reset();
    }

    private async Task SendConfigAsync()
    {
        var settings = _settingsStore.Load();
        await _bridge.SendAsync("CONFIG", BuildConfigValues(settings)).ConfigureAwait(false);
    }

    private IReadOnlyDictionary<string, string> BuildConfigValues(LauncherSettings settings) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["mode"] = State.IsHost ? "Host" : "Client",
            ["playerName"] = settings.PlayerName,
            ["serverName"] = $"{settings.PlayerName}'s Lobby",
            ["serverPassword"] = string.Empty,
            ["hostIp"] = settings.HostAddress,
            ["port"] = settings.Port.ToString(CultureInfo.InvariantCulture),
            ["maxPlayers"] = ProtocolConstants.DefaultLobbyCapacity.ToString(CultureInfo.InvariantCulture),
            ["gameMode"] = "Survival",
            ["visibility"] = "Private",
            ["survivorMapId"] = "ChallengeMap16",
            ["survivorMapName"] = "ChallengeMap16"
        };

    private async Task SendCurrentLobbyAsync()
    {
        LobbySnapshotPayload? snapshot;
        lock (_stateGate) snapshot = _lobby;
        if (snapshot is not null) await SendLobbyStateAsync(snapshot).ConfigureAwait(false);
    }

    private Task SendLobbyStateAsync(LobbySnapshotPayload snapshot) =>
        _bridge.SendAsync("LOBBY_STATE", BuildLobbyValues(snapshot));

    private IReadOnlyDictionary<string, string> BuildLobbyValues(LobbySnapshotPayload snapshot)
    {
        var players = snapshot.Players.OrderByDescending(value => value.IsHost)
            .ThenBy(value => value.PeerId).ToArray();
        return new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["role"] = State.IsHost ? "host" : "client",
            ["status"] = snapshot.State == "Starting" ? "Starting" : "Waiting for players",
            ["mode"] = "Survival",
            ["mapId"] = snapshot.Level,
            ["mapName"] = snapshot.Level,
            ["p1"] = FormatPlayer(players, 0, 1),
            ["p2"] = FormatPlayer(players, 1, 2),
            ["p3"] = "3  Empty",
            ["p4"] = "4  Empty"
        };
    }

    private static string FormatPlayer(
        IReadOnlyList<LobbyPlayer> players,
        int index,
        int slot)
    {
        if (index >= players.Count) return $"{slot}  Empty";
        var player = players[index];
        var role = player.IsHost ? "Owner" : "Player";
        var ready = player.Ready ? "Ready" : "Waiting";
        return $"{slot}  {role}  {player.DisplayName}  {ready}";
    }

    private Task SendStartGameAsync(LobbyStartPayload start)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["sessionId"] = start.SessionId.ToString("N"),
            ["sessionGeneration"] = start.LevelGeneration.ToString(CultureInfo.InvariantCulture),
            ["lobbyId"] = start.SessionId.ToString("N"),
            ["syncProtocol"] = ProtocolConstants.Version.ToString(CultureInfo.InvariantCulture),
            ["mode"] = "Survival",
            ["mapId"] = start.Level,
            ["mapName"] = start.Level,
            ["hostId"] = ResolveHostId(),
            ["status"] = "starting"
        };
        return _bridge.SendAsync("START_GAME", values);
    }

    private string ResolveHostId()
    {
        lock (_stateGate)
        {
            var host = _lobby?.Players.FirstOrDefault(value => value.IsHost);
            return host?.LauncherPlayerId.ToString("N") ?? string.Empty;
        }
    }

    private Task SendStatusAsync(string message) => _bridge.SendAsync(
        "STATUS", new Dictionary<string, string> { ["message"] = message });

    private Task SendErrorAsync(string message) => _bridge.SendAsync(
        "ERROR", new Dictionary<string, string> { ["message"] = message });

    private Task LogOnlyAsync(string message)
    {
        _diagnostics.Log("Multiplayer", message);
        return Task.CompletedTask;
    }

    private async Task ReportBridgeFailureAsync(Exception exception)
    {
        _diagnostics.Log("Multiplayer", $"Command failed: {exception.Message}");
        await SendErrorAsync(exception.Message).ConfigureAwait(false);
    }

    private MultiplayerSessionView CreateView()
    {
        var role = _host is not null ? "Hosting" : _client is not null ? "Connected" : "Disconnected";
        var remote = _lobby?.Players.FirstOrDefault(value => !IsLocalPlayer(value));
        return new MultiplayerSessionView(role, _host is not null, _host is not null || _client is not null,
            _localGameplayReady, _remoteGameplayReady, remote?.DisplayName ?? string.Empty);
    }

    private ulong LocalPeerId() => _host?.HostPeerId ?? _client?.PeerId ?? 0;

    private bool IsLocalPlayer(LobbyPlayer player) => player.PeerId == LocalPeerId();

    private void PublishState() => StateChanged?.Invoke(this, State);
}

internal sealed record RemoteMovementTarget(
    ulong PeerId,
    uint CharacterBase,
    int LevelGeneration,
    RemotePositionTrackSnapshot Track);

internal static class LobbyCompatibility
{
    private const string ProfileId = "aimp-ws3-logicswitch-lifecycle-v33-root10-wm08c9";

    private static string EffectiveProfileId(LauncherSettings settings) => ProfileId +
        ":ws3" +
        (settings.WorldSync3EmptyCallbackEnabled ? ":probe-callback-test" : ":strict");

    internal static CompatibilityInfo Create(LauncherSettings settings)
    {
        var profileId = EffectiveProfileId(settings);
        return new CompatibilityInfo(
            ProtocolConstants.Version,
            ProductInfo.LauncherVersion,
            ProductInfo.ExpectedDllVersion,
            ProtocolConstants.HistoricalAiSha256,
            profileId,
            ComputeHash(profileId));
    }

    private static string ComputeHash(string profileId)
    {
        var text = $"{ProtocolConstants.Version}|{ProductInfo.LauncherVersion}|" +
            $"{ProductInfo.ExpectedDllVersion}|{profileId}";
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
    }
}


internal sealed record RemotePlayerSpawnTarget(
    LobbyPlayer Player,
    PlayerSpawnPositionPayload Position);

internal static class RemoteSpawnPlanner
{
    internal static bool AllRemotePlayersReady(
        LobbySnapshotPayload? lobby,
        ulong localPeerId,
        IReadOnlySet<ulong> readyPeers)
    {
        var remotePeers = (lobby?.Players ?? Array.Empty<LobbyPlayer>())
            .Where(player => player.PeerId != localPeerId)
            .Select(player => player.PeerId)
            .ToArray();
        return remotePeers.Length > 0
            && remotePeers.All(readyPeers.Contains);
    }

    internal static IReadOnlyList<RemotePlayerSpawnTarget> SelectPendingPlayers(
        LobbySnapshotPayload? lobby,
        IReadOnlyList<PlayerSpawnPositionPayload>? positions,
        ulong localPeerId,
        IReadOnlyDictionary<ulong, uint> existing)
    {
        var byPeer = (positions ?? Array.Empty<PlayerSpawnPositionPayload>())
            .ToDictionary(value => value.PeerId);
        return (lobby?.Players ?? Array.Empty<LobbyPlayer>())
            .Where(player => player.PeerId != localPeerId)
            .Where(player => !existing.ContainsKey(player.PeerId))
            .Where(player => byPeer.ContainsKey(player.PeerId))
            .OrderBy(player => player.PeerId)
            .Select(player => new RemotePlayerSpawnTarget(player, byPeer[player.PeerId]))
            .ToArray();
    }

    internal static bool MatchesLobby(
        IReadOnlyList<PlayerSpawnPositionPayload> positions,
        IReadOnlyList<LobbyPlayer> players) =>
        positions.Select(value => value.PeerId).OrderBy(value => value)
            .SequenceEqual(players.Select(value => value.PeerId).OrderBy(value => value));

    internal static PlayerSpawnPositionPayload CreateNetworkPosition(
        ulong peerId,
        LocalPlayerPosition position) =>
        new(peerId, position.X, position.Y, position.Z);

    internal static PlayerSpawnPositionPayload CreateNetworkPosition(
        PlayerPositionSnapshotPayload position) =>
        new(position.PeerId, position.X, position.Y, position.Z);

    internal static IReadOnlyDictionary<string, string> Create(
        PlayerSpawnPositionPayload position) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["x"] = position.X.ToString("0.000", CultureInfo.InvariantCulture),
            ["y"] = position.Y.ToString("0.000", CultureInfo.InvariantCulture),
            ["z"] = position.Z.ToString("0.000", CultureInfo.InvariantCulture)
        };

    internal static IReadOnlyDictionary<string, string> CreateJoinMessage(
        string playerName) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["headerText"] = "AIMP",
            ["mainText"] = $"{playerName} join the game",
            ["duration"] = "5.0",
            ["icon"] = PopupMessageIcons.Alert,
            ["soundEvent"] = PopupMessageSoundEvents.ObjectiveNew
        };
}
