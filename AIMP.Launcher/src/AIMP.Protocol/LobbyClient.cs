using System.Net.Sockets;
using System.Security.Cryptography;

namespace AIMP.Protocol;

public sealed record LobbyClientOptions(
    string Host,
    int Port,
    Guid LauncherPlayerId,
    string DisplayName,
    CompatibilityInfo Compatibility);

public sealed partial class LobbyClient : IAsyncDisposable
{
    private static readonly TimeSpan HeartbeatPeriod = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan HostTimeout = TimeSpan.FromSeconds(10);

    private readonly ProtocolConnection _connection;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Dictionary<ulong, PlayerPositionReceiveWindow> _positionWindows = new();
    private readonly long _connectionGeneration;
    private Task? _readLoop;
    private Task? _heartbeatLoop;
    private int _started;
    private int _disposing;
    private long _lastHostSeenTicks = Environment.TickCount64;

    private LobbyClient(
        ProtocolConnection connection,
        LobbyJoinAcceptedPayload accepted,
        long connectionGeneration)
    {
        _connection = connection;
        SessionId = accepted.SessionId;
        PeerId = accepted.PeerId;
        Capacity = accepted.Capacity;
        OpaqueRelayEnabled = accepted.OpaqueRelayEnabled;
        _connectionGeneration = connectionGeneration;
    }

    public Guid SessionId { get; }

    public ulong PeerId { get; }

    public int Capacity { get; }

    public bool OpaqueRelayEnabled { get; }

    public LobbySnapshotPayload? Snapshot { get; private set; }

    public event EventHandler<LobbySnapshotPayload>? SnapshotChanged;

    public event EventHandler<LobbyStartPayload>? Started;

    public event EventHandler<GameplayReadyPayload>? GameplayReadyReceived;

    public event EventHandler<RoundEndedPayload>? RoundEnded;

    public event EventHandler<ItemCollectedPayload>? ItemCollected;

    public event EventHandler<ItemSpawnPayload>? ItemSpawnDecided;

    public event EventHandler<SpawnPositionRequestPayload>? SpawnPositionRequested;

    public event EventHandler<SpawnRemotePlayersPayload>? SpawnRemotePlayersRequested;

    public event EventHandler<PlayerPositionSnapshotPayload>? PlayerPositionReceived;

    public event EventHandler<PlayerEquipmentSnapshotPayload>? PlayerEquipmentReceived;

    public event EventHandler<PlayerActionEventPayload>? PlayerActionReceived;


    public event EventHandler<WorldNetworkContext>? WorldContextReceived;

    public event EventHandler<WorldNetworkMutation>? WorldMutationReceived;

    public event EventHandler<WorldNetworkIntent>? WorldIntentRejected;

    public event EventHandler<WorldTransactionRecoveryStatus>? WorldRecoveryStatusReceived;

    public event EventHandler<ErrorPayload>? ErrorReceived;

    public event EventHandler<ReadOnlyMemory<byte>>? OpaqueFrameReceived;

    public event EventHandler<string>? Log;

    public event EventHandler? Disconnected;

    public static async Task<LobbyClient> ConnectAsync(
        LobbyClientOptions options,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (!PlayerName.TryNormalize(options.DisplayName, out var displayName, out var error))
        {
            throw new ArgumentException(error, nameof(options));
        }

        if (options.Port is < 1 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(options));
        }

        if (string.IsNullOrWhiteSpace(options.Host) || options.Host.Length > 253)
        {
            throw new ArgumentException("The host address is invalid.", nameof(options));
        }

        ModelValidation.Validate(options.Compatibility);
        if (options.LauncherPlayerId == Guid.Empty)
        {
            throw new ArgumentException("LauncherPlayerId is invalid.", nameof(options));
        }

        var tcpClient = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(7));
            await tcpClient.ConnectAsync(options.Host, options.Port, timeout.Token)
                .ConfigureAwait(false);
            var connection = new ProtocolConnection(tcpClient);

            var generation = RandomNumberGenerator.GetInt32(1, int.MaxValue);
            var request = new LobbyJoinPayload(
                options.LauncherPlayerId,
                displayName,
                options.Compatibility,
                generation);
            await connection.SendAsync(MessageType.LobbyJoin, JsonPayload.Serialize(request),
                timeout.Token).ConfigureAwait(false);

            var response = await connection.ReadAsync(timeout.Token).ConfigureAwait(false);
            if (response.Type == MessageType.Error)
            {
                var protocolError = JsonPayload.Deserialize<ErrorPayload>(response.Payload.Span);
                ModelValidation.Validate(protocolError);
                throw new ProtocolException($"{protocolError.Code}: {protocolError.Message}");
            }

            if (response.Type != MessageType.LobbyJoinAccepted)
            {
                throw new ProtocolException("Expected LOBBY_JOIN_ACCEPTED.");
            }

            var accepted = JsonPayload.Deserialize<LobbyJoinAcceptedPayload>(response.Payload.Span);
            ModelValidation.Validate(accepted);
            if (accepted.ConnectionGeneration != generation)
            {
                throw new ProtocolException("The connection generation is invalid.");
            }

            var client = new LobbyClient(connection, accepted, generation);
            var initialFrame = await connection.ReadAsync(timeout.Token).ConfigureAwait(false);
            if (initialFrame.Type != MessageType.LobbySnapshot)
            {
                throw new ProtocolException("Expected an initial LOBBY_SNAPSHOT.");
            }

            client.Snapshot = JsonPayload.Deserialize<LobbySnapshotPayload>(
                initialFrame.Payload.Span);
            ModelValidation.Validate(client.Snapshot);
            if (client.Snapshot.SessionId != accepted.SessionId)
            {
                throw new ProtocolException("The initial snapshot SessionId is invalid.");
            }

            return client;
        }
        catch
        {
            tcpClient.Dispose();
            throw;
        }
    }

    public void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0)
        {
            throw new InvalidOperationException("The lobby client is already running.");
        }

        _readLoop = ReadLoopAsync(_shutdown.Token);
        _heartbeatLoop = HeartbeatLoopAsync(_shutdown.Token);
    }

    public Task SetReadyAsync(bool ready, CancellationToken cancellationToken = default) =>
        _connection.SendAsync(
            MessageType.LobbyReady,
            JsonPayload.Serialize(new LobbyReadyPayload(ready)),
            cancellationToken).AsTask();

    public Task SendGameplayReadyAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = new GameplayReadyPayload(SessionId, PeerId, levelGeneration);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(
            MessageType.GameplayReady,
            JsonPayload.Serialize(payload),
            cancellationToken).AsTask();
    }

    public Task SendPlayerDeathAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = new PlayerDeathPayload(SessionId, PeerId, levelGeneration);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(
            MessageType.PlayerDeath,
            JsonPayload.Serialize(payload),
            cancellationToken).AsTask();
    }

    public Task SendItemPickupAsync(
        int levelGeneration,
        string entityId,
        CancellationToken cancellationToken = default)
    {
        var payload = new ItemPickupRequestPayload(SessionId, PeerId, levelGeneration, entityId);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(
            MessageType.ItemPickupRequest,
            JsonPayload.Serialize(payload),
            cancellationToken).AsTask();
    }

    public Task SendItemSpawnProposalAsync(
        int levelGeneration,
        NativeItemSyncSpawnPayload value,
        CancellationToken cancellationToken = default)
    {
        var payload = new ItemSpawnPayload(SessionId, PeerId, levelGeneration, value.Kind,
            value.OwnerKey, value.Spawn, value.Path, value.Quantity, value.Transform);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(
            MessageType.ItemSpawnProposal,
            JsonPayload.Serialize(payload),
            cancellationToken).AsTask();
    }

    public Task SendSpawnPositionAsync(
        SpawnPositionRequestPayload request,
        PlayerSpawnPositionPayload position,
        CancellationToken cancellationToken = default)
    {
        ValidateSpawnPositionResponseInput(request, position);
        var payload = new SpawnPositionResponsePayload(SessionId, request.RequestId, PeerId,
            request.LevelGeneration, position.X, position.Y, position.Z);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(MessageType.SpawnPositionResponse,
            JsonPayload.Serialize(payload), cancellationToken).AsTask();
    }

    public Task SendPlayerPositionAsync(
        PlayerPositionSnapshotPayload payload,
        CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != PeerId)
            throw new InvalidOperationException("The player-position identity is invalid.");
        return _connection.SendAsync(MessageType.PlayerPositionSnapshot,
            JsonPayload.Serialize(payload), cancellationToken).AsTask();
    }

    public Task SendPlayerEquipmentAsync(
        PlayerEquipmentSnapshotPayload payload,
        CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != PeerId)
            throw new InvalidOperationException("The player-equipment identity is invalid.");
        return _connection.SendAsync(MessageType.PlayerEquipmentSnapshot,
            JsonPayload.Serialize(payload), cancellationToken).AsTask();
    }

    public Task SendPlayerActionAsync(
        PlayerActionEventPayload payload,
        CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != PeerId)
            throw new InvalidOperationException("The player-action identity is invalid.");
        return _connection.SendAsync(MessageType.PlayerActionEvent,
            JsonPayload.Serialize(payload), cancellationToken).AsTask();
    }

    public Task SendAISyncImpactAsync(
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken = default)
    {
        var native = AISyncCodec.DecodeImpactNetwork(bytes, SessionId);
        if (AISyncCodec.ImpactSourcePeer(native.Span) != PeerId)
            throw new InvalidOperationException("The AISync impact source identity is invalid.");
        return _connection.SendAsync(MessageType.AISyncNetworkImpact,
            bytes, cancellationToken).AsTask();
    }

    private void ValidateSpawnPositionResponseInput(
        SpawnPositionRequestPayload request,
        PlayerSpawnPositionPayload position)
    {
        ModelValidation.Validate(request);
        ModelValidation.Validate(position);
        if (request.SessionId != SessionId || position.PeerId != PeerId)
            throw new InvalidOperationException("The spawn-position response identity is invalid.");
    }

    public Task SendWorldContextAsync(
        WorldNetworkContext context,
        CancellationToken cancellationToken = default)
    {
        WorldReplicationValidation.Validate(context);
        if (context.SessionId != SessionId || context.PeerId != PeerId)
            throw new InvalidOperationException("The WorldContext identity is invalid.");
        return _connection.SendAsync(MessageType.WorldContext,
            WorldReplicationCodec.EncodeNetworkContext(context), cancellationToken).AsTask();
    }

    public Task SendWorldIntentAsync(
        WorldNetworkIntent intent,
        CancellationToken cancellationToken = default)
    {
        WorldReplicationValidation.Validate(intent);
        if (intent.SessionId != SessionId || intent.Intent.SourcePeer != PeerId)
            throw new InvalidOperationException("The WorldIntent identity is invalid.");
        return _connection.SendAsync(MessageType.WorldNetworkIntent,
            WorldReplicationCodec.EncodeNetworkIntent(intent), cancellationToken).AsTask();
    }

    public Task SendWorldMutationAckAsync(
        WorldNetworkMutationAck ack,
        CancellationToken cancellationToken = default)
    {
        WorldReplicationValidation.Validate(ack);
        if (ack.SessionId != SessionId || ack.PeerId != PeerId)
            throw new InvalidOperationException("The WorldMutation acknowledgement identity is invalid.");
        return _connection.SendAsync(MessageType.WorldNetworkMutationAck,
            WorldReplicationCodec.EncodeNetworkAck(ack), cancellationToken).AsTask();
    }

    public Task SendWorldTransactionRecoveryRequestAsync(
        WorldTransactionRecoveryRequest request,
        CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(request);
        if (request.SessionId != SessionId || request.PeerId != PeerId)
            throw new InvalidOperationException("The world recovery request identity is invalid.");
        return _connection.SendAsync(MessageType.WorldTransactionRecoveryRequest,
            JsonPayload.Serialize(request), cancellationToken).AsTask();
    }

    public Task SendOpaqueFrameAsync(
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken = default)
    {
        if (!OpaqueRelayEnabled)
        {
            throw new InvalidOperationException(
                "Opaque relay is disabled for this session.");
        }

        return _connection.SendAsync(MessageType.OutboundNetZFrame, payload, cancellationToken)
            .AsTask();
    }

    private async Task ReadLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var frame = await _connection.ReadAsync(cancellationToken).ConfigureAwait(false);
                Interlocked.Exchange(ref _lastHostSeenTicks, Environment.TickCount64);
                switch (frame.Type)
                {
                    case MessageType.AISyncExecutionControl:
                        await HandleAISyncExecutionControlAsync(frame.Payload).ConfigureAwait(false);
                        break;
                    case MessageType.AISyncExecutionNetworkBatch:
                        HandleAISyncExecutionBatch(frame.Payload);
                        break;
                    case MessageType.AISyncCausalStateNetwork:
                        HandleAISyncCausalState(frame.Payload);
                        break;
                    case MessageType.AISyncRouteNetwork:
                        HandleAISyncRoute(frame.Payload);
                        break;
                    case MessageType.AISyncExecutionNetworkRetry:
                        HandleAISyncExecutionBatch(frame.Payload, retry: true);
                        break;
                    case MessageType.AISyncExecutionNetworkReceipt:
                        throw new ProtocolException("Only a Client may send executable AISync receipts.");
                    case MessageType.AISyncShadowOpen:
                        await HandleAISyncShadowOpenAsync(frame.Payload, cancellationToken).ConfigureAwait(false);
                        break;
                    case MessageType.AISyncShadowData:
                        await HandleAISyncShadowDataAsync(frame.Payload, cancellationToken).ConfigureAwait(false);
                        break;
                    case MessageType.AISyncShadowRecovery:
                        HandleAISyncShadowRecovery(frame.Payload);
                        break;
                    case MessageType.AISyncShadowReceipt:
                        throw new ProtocolException("Only a shadow consumer may send receipts.");
                    case MessageType.VoiceFrame:
                        HandleVoiceFrame(frame.Payload);
                        break;
                    case MessageType.MissionSignalCommitted:
                        HandleMissionSignalCommitted(frame.Payload);
                        break;
                    case MessageType.Heartbeat:
                        break;

                    case MessageType.LobbySnapshot:
                        Snapshot = JsonPayload.Deserialize<LobbySnapshotPayload>(frame.Payload.Span);
                        ModelValidation.Validate(Snapshot);
                        if (Snapshot.SessionId != SessionId)
                        {
                            throw new ProtocolException("The snapshot SessionId is invalid.");
                        }

                        Log?.Invoke(this,
                            $"Lobby snapshot updated: state={Snapshot.State}, generation={Snapshot.LevelGeneration}, players={Snapshot.Players.Count}.");
                        SnapshotChanged?.Invoke(this, Snapshot);
                        break;

                    case MessageType.LobbyStart:
                        var start = JsonPayload.Deserialize<LobbyStartPayload>(frame.Payload.Span);
                        ModelValidation.Validate(start);
                        if (start.SessionId != SessionId)
                        {
                            throw new ProtocolException("The launch SessionId is invalid.");
                        }

                        Started?.Invoke(this, start);
                        break;

                    case MessageType.LobbyPreparation:
                        var preparation = JsonPayload.Deserialize<LobbyPreparationPayload>(frame.Payload.Span);
                        ModelValidation.Validate(preparation);
                        if (preparation.SessionId != SessionId)
                            throw new ProtocolException("The lobby preparation session is invalid.");
                        LobbyPreparationReceived?.Invoke(this, preparation);
                        break;

                    case MessageType.SessionRelease:
                        var release = JsonPayload.Deserialize<SessionReleasePayload>(frame.Payload.Span);
                        ModelValidation.Validate(release);
                        if (release.SessionId != SessionId)
                            throw new ProtocolException("The session release identity is invalid.");
                        if (release.LevelGeneration == Snapshot?.LevelGeneration)
                            SessionReleased?.Invoke(this, release);
                        break;

                    case MessageType.GameplayReady:
                        var gameplay = JsonPayload.Deserialize<GameplayReadyPayload>(
                            frame.Payload.Span);
                        ModelValidation.Validate(gameplay);
                        if (gameplay.SessionId != SessionId)
                        {
                            throw new ProtocolException(
                                "The gameplay-ready SessionId is invalid.");
                        }

                        GameplayReadyReceived?.Invoke(this, gameplay);
                        break;

                    case MessageType.SpawnPositionRequest:
                        var positionRequest = JsonPayload.Deserialize<SpawnPositionRequestPayload>(
                            frame.Payload.Span);
                        ValidateSpawnPositionRequest(positionRequest);
                        SpawnPositionRequested?.Invoke(this, positionRequest);
                        break;

                    case MessageType.SpawnRemotePlayers:
                        var spawn = JsonPayload.Deserialize<SpawnRemotePlayersPayload>(
                            frame.Payload.Span);
                        ValidateSpawnRemotePlayers(spawn);
                        SpawnRemotePlayersRequested?.Invoke(this, spawn);
                        break;

                    case MessageType.PlayerPositionSnapshot:
                        var position = JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(
                            frame.Payload.Span);
                        ValidatePlayerPosition(position);
                        if (!AcceptPlayerPosition(position)) break;
                        PlayerPositionReceived?.Invoke(this, position);
                        break;

                    case MessageType.PlayerEquipmentSnapshot:
                        var equipment = JsonPayload.Deserialize<PlayerEquipmentSnapshotPayload>(
                            frame.Payload.Span);
                        ValidatePlayerEquipment(equipment);
                        PlayerEquipmentReceived?.Invoke(this, equipment);
                        break;

                    case MessageType.PlayerActionEvent:
                        var action = JsonPayload.Deserialize<PlayerActionEventPayload>(
                            frame.Payload.Span);
                        ValidatePlayerAction(action);
                        PlayerActionReceived?.Invoke(this, action);
                        break;

                    case MessageType.AISyncNetworkState:
                        throw new ProtocolException("Independent AISync state traffic is retired.");

                    case MessageType.WorldContext:
                        var worldContext = WorldReplicationCodec.DecodeNetworkContext(frame.Payload.Span);
                        if (worldContext.SessionId != SessionId)
                            throw new ProtocolException("WorldContext SessionId is invalid.");
                        var hostPeerId = Snapshot?.Players.SingleOrDefault(player => player.IsHost)?.PeerId ?? 0;
                        if (worldContext.PeerId != hostPeerId)
                            throw new ProtocolException("WorldContext did not originate from the host.");
                        WorldContextReceived?.Invoke(this, worldContext);
                        break;

                    case MessageType.WorldNetworkMutation:
                        var worldMutation = WorldReplicationCodec.DecodeNetworkMutation(frame.Payload.Span);
                        if (worldMutation.SessionId != SessionId)
                            throw new ProtocolException("WorldMutation SessionId is invalid.");
                        WorldMutationReceived?.Invoke(this, worldMutation);
                        break;

                    case MessageType.WorldNetworkIntentRejected:
                        var rejectedIntent = WorldReplicationCodec.DecodeNetworkIntent(frame.Payload.Span);
                        if (rejectedIntent.SessionId != SessionId
                            || rejectedIntent.Intent.SourcePeer != PeerId
                            || rejectedIntent.Intent.Operation != WorldReplicationConstants.CompositePublicInputOperation
                            || rejectedIntent.Intent.RequestedKind != WorldMutationKind.Event
                            || !WorldPublicActionCodec.TryDecode(rejectedIntent.Intent.Payload,
                                out _, out _, out var rejectedRetailRoot)
                            || !rejectedRetailRoot)
                            throw new ProtocolException("WorldIntent rejection identity or v2 ROOT contract is invalid.");
                        WorldIntentRejected?.Invoke(this, rejectedIntent);
                        break;

                    case MessageType.WorldTransactionRecoveryStatus:
                        var recoveryStatus = JsonPayload.Deserialize<WorldTransactionRecoveryStatus>(
                            frame.Payload.Span);
                        ModelValidation.Validate(recoveryStatus);
                        if (recoveryStatus.SessionId != SessionId || recoveryStatus.PeerId != PeerId)
                            throw new ProtocolException("World recovery status identity is invalid.");
                        WorldRecoveryStatusReceived?.Invoke(this, recoveryStatus);
                        break;

                    case MessageType.RoundProgress:
                        HandleRoundProgress(frame.Payload);
                        break;

                    case MessageType.RoundEnded:
                        var roundEnded = JsonPayload.Deserialize<RoundEndedPayload>(frame.Payload.Span);
                        ModelValidation.Validate(roundEnded);
                        if (roundEnded.SessionId != SessionId)
                            throw new ProtocolException("The round-ended SessionId is invalid.");
                        await StopShadowControlsAsync().ConfigureAwait(false);
                        ClearAISyncShadow();
                        await ClearAISyncExecutionAsync().ConfigureAwait(false);
                        RoundEnded?.Invoke(this, roundEnded);
                        break;

                    case MessageType.ItemCollected:
                        var itemCollected = JsonPayload.Deserialize<ItemCollectedPayload>(frame.Payload.Span);
                        ModelValidation.Validate(itemCollected);
                        if (itemCollected.SessionId != SessionId)
                            throw new ProtocolException("The item-collected SessionId is invalid.");
                        ItemCollected?.Invoke(this, itemCollected);
                        break;

                    case MessageType.ItemSpawnDecision:
                        var itemSpawn = JsonPayload.Deserialize<ItemSpawnPayload>(frame.Payload.Span);
                        ModelValidation.Validate(itemSpawn);
                        if (itemSpawn.SessionId != SessionId)
                            throw new ProtocolException("The item-spawn SessionId is invalid.");
                        ItemSpawnDecided?.Invoke(this, itemSpawn);
                        break;

                    case MessageType.ThrowableCommit:
                        HandleThrowableCommit(frame.Payload);
                        break;

                    case MessageType.InboundNetZFrame:
                        OpaqueFrameReceived?.Invoke(this, frame.Payload);
                        break;

                    case MessageType.Error:
                        var error = JsonPayload.Deserialize<ErrorPayload>(frame.Payload.Span);
                        ModelValidation.Validate(error);
                        ErrorReceived?.Invoke(this, error);
                        break;

                    case MessageType.Shutdown:
                    case MessageType.LobbyLeave:
                        return;

                    default:
                        throw new ProtocolException(
                        $"Unexpected host message: {frame.Type}.");
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (cancellationToken.IsCancellationRequested)
        {
            // Dispose cancels then closes the socket to unblock its reader.
            // The next ReadAsync can observe closure before cancellation.
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or EndOfStreamException or ObjectDisposedException
                or ProtocolException)
        {
            Log?.Invoke(this, $"Lobby connection ended: {exception.Message}");
        }
        finally
        {
            await StopShadowControlsAsync().ConfigureAwait(false);
            ClearAISyncShadow();
            await ClearAISyncExecutionAsync().ConfigureAwait(false);
            if (Volatile.Read(ref _disposing) == 0)
            {
                Disconnected?.Invoke(this, EventArgs.Empty);
            }
        }
    }

    private void ValidateSpawnRemotePlayers(SpawnRemotePlayersPayload payload)
    {
        ModelValidation.Validate(payload);
        var hostPeerId = Snapshot?.Players.SingleOrDefault(player => player.IsHost)?.PeerId ?? 0;
        if (payload.SessionId != SessionId || payload.HostPeerId != hostPeerId)
            throw new ProtocolException("The remote-player spawn request identity is invalid.");
    }

    private void ValidateSpawnPositionRequest(SpawnPositionRequestPayload payload)
    {
        ModelValidation.Validate(payload);
        var hostPeerId = Snapshot?.Players.SingleOrDefault(player => player.IsHost)?.PeerId ?? 0;
        if (payload.SessionId != SessionId || payload.HostPeerId != hostPeerId)
            throw new ProtocolException("The spawn-position request identity is invalid.");
    }

    private void ValidatePlayerPosition(PlayerPositionSnapshotPayload payload)
    {
        ModelValidation.Validate(payload);
        var known = Snapshot?.Players.Any(player => player.PeerId == payload.PeerId) == true;
        if (payload.SessionId != SessionId || !known || payload.PeerId == PeerId)
            throw new ProtocolException("The player-position snapshot identity is invalid.");
    }

    private bool AcceptPlayerPosition(PlayerPositionSnapshotPayload value)
    {
        if (!_positionWindows.TryGetValue(value.PeerId, out var window))
            _positionWindows[value.PeerId] = window = new PlayerPositionReceiveWindow();
        return window.TryAccept(value);
    }

    private void ValidatePlayerEquipment(PlayerEquipmentSnapshotPayload payload)
    {
        ModelValidation.Validate(payload);
        var known = Snapshot?.Players.Any(player => player.PeerId == payload.PeerId) == true;
        if (payload.SessionId != SessionId || !known || payload.PeerId == PeerId)
            throw new ProtocolException("The player-equipment snapshot identity is invalid.");
    }

    private void ValidatePlayerAction(PlayerActionEventPayload payload)
    {
        ModelValidation.Validate(payload);
        var known = Snapshot?.Players.Any(player => player.PeerId == payload.PeerId) == true;
        if (payload.SessionId != SessionId || !known || payload.PeerId == PeerId)
            throw new ProtocolException("The player-action event identity is invalid.");
    }

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(HeartbeatPeriod);
        try
        {
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                if (Environment.TickCount64 - Interlocked.Read(ref _lastHostSeenTicks)
                    > HostTimeout.TotalMilliseconds)
                {
                    Log?.Invoke(this,
                        "Host heartbeat timed out after 10 seconds; connection closed.");
                    _connection.Client.Dispose();
                    return;
                }

                await _connection.SendAsync(
                    MessageType.Heartbeat,
                    JsonPayload.Serialize(new { generation = _connectionGeneration }),
                    cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or ObjectDisposedException)
        {
            Log?.Invoke(this, $"Heartbeat stopped: {exception.Message}");
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposing, 1) != 0)
        {
            return;
        }

        try
        {
            using var leaveTimeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
            await _connection.SendAsync(
                MessageType.LobbyLeave,
                ReadOnlyMemory<byte>.Empty,
                leaveTimeout.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or ObjectDisposedException
                or OperationCanceledException)
        {
        }

        _shutdown.Cancel();
        _connection.Client.Dispose();
        foreach (var task in new[] { _readLoop, _heartbeatLoop }.OfType<Task>())
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        await _connection.DisposeAsync().ConfigureAwait(false);
        _shutdown.Dispose();
    }
}
