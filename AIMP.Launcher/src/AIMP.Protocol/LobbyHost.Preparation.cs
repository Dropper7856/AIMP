namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private Guid _preparationToken = Guid.NewGuid();
    private readonly Dictionary<ulong, LobbyPreparationPayload> _lobbyPreparations = new();
    private readonly HashSet<ulong> _sessionPreparedPeers = new();
    private int _sessionReleasedGeneration;

    public event EventHandler<LobbyPreparationPayload>? LobbyPreparationReceived;
    public event EventHandler<SessionPreparedPayload>? SessionPreparedReceived;
    public event EventHandler<SessionReleasePayload>? SessionReleased;

    // Explicit Start preflight: even an unchanged level must be checked again
    // after files may have changed since opening the lobby or the previous round.
    public void RefreshWorldPreparation()
    {
        lock (_stateLock)
        {
            if (!string.Equals(_state, "Lobby", StringComparison.Ordinal))
                throw new InvalidOperationException("World preparation cannot refresh after launch.");
            _preparationToken = Guid.NewGuid();
            _lobbyPreparations.Clear();
        }
        PublishSnapshot();
        _ = BroadcastSnapshotSafelyAsync();
    }

    public async Task SetHostPreparationAsync(
        LobbyPreparationPayload preparation, CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(preparation);
        if (preparation.SessionId != SessionId || preparation.PeerId != HostPeerId)
            throw new InvalidOperationException("Host preparation identity is invalid.");
        lock (_stateLock)
        {
            if (!AcceptLobbyPreparationLocked(preparation))
                throw new InvalidOperationException("Host preparation targets a stale lobby selection.");
        }
        LobbyPreparationReceived?.Invoke(this, preparation);
        await BroadcastAsync(MessageType.LobbyPreparation, JsonPayload.Serialize(preparation),
            cancellationToken).ConfigureAwait(false);
        PublishSnapshot();
        await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    public Task SetHostSessionPreparedAsync(
        int levelGeneration, CancellationToken cancellationToken = default) =>
        AcceptSessionPreparedAsync(new SessionPreparedPayload(SessionId, HostPeerId, levelGeneration),
            null, cancellationToken);

    // Called under the same state lock as StartGameAsync's final admission check.
    private bool AllLobbyPreparationsMatchLocked()
    {
        if (!_lobbyPreparations.TryGetValue(HostPeerId, out var host) || !host.Ready
            || host.PreparationToken != _preparationToken
            || !string.Equals(host.Level, _level, StringComparison.OrdinalIgnoreCase)) return false;
        return _clients.Keys.All(peerId =>
            _lobbyPreparations.TryGetValue(peerId, out var peer) && peer.Ready
            && peer.PreparationToken == _preparationToken
            && string.Equals(peer.Level, _level, StringComparison.OrdinalIgnoreCase)
            && peer.Manifest == host.Manifest
            && string.Equals(peer.ManifestSha256, host.ManifestSha256, StringComparison.OrdinalIgnoreCase));
    }

    private bool AcceptLobbyPreparationLocked(LobbyPreparationPayload preparation)
    {
        if (!string.Equals(_state, "Lobby", StringComparison.Ordinal)
            || preparation.PreparationToken != _preparationToken
            || !string.Equals(preparation.Level, _level, StringComparison.OrdinalIgnoreCase)) return false;
        _lobbyPreparations[preparation.PeerId] = preparation;
        return true;
    }

    private async Task HandleLobbyPreparationAsync(
        ClientState state, ReadOnlyMemory<byte> bytes, CancellationToken cancellationToken)
    {
        var preparation = JsonPayload.Deserialize<LobbyPreparationPayload>(bytes.Span);
        ModelValidation.Validate(preparation);
        if (preparation.SessionId != SessionId || preparation.PeerId != state.PeerId)
            throw new ProtocolException("Lobby preparation does not identify its authenticated sender.");
        lock (_stateLock)
        {
            if (!_clients.ContainsKey(state.PeerId) || !AcceptLobbyPreparationLocked(preparation)) return;
        }
        LobbyPreparationReceived?.Invoke(this, preparation);
        await BroadcastAsync(MessageType.LobbyPreparation, JsonPayload.Serialize(preparation),
            cancellationToken).ConfigureAwait(false);
        PublishSnapshot();
        await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    private Task HandleSessionPreparedAsync(
        ClientState state, ReadOnlyMemory<byte> bytes, CancellationToken cancellationToken)
    {
        var prepared = JsonPayload.Deserialize<SessionPreparedPayload>(bytes.Span);
        ModelValidation.Validate(prepared);
        if (prepared.SessionId != SessionId || prepared.PeerId != state.PeerId)
            throw new ProtocolException("Session readiness does not identify its authenticated sender.");
        return AcceptSessionPreparedAsync(prepared, state, cancellationToken);
    }

    private async Task AcceptSessionPreparedAsync(
        SessionPreparedPayload prepared, ClientState? sender, CancellationToken cancellationToken)
    {
        ModelValidation.Validate(prepared);
        SessionReleasePayload? release = null;
        var replayRelease = false;
        lock (_stateLock)
        {
            if (prepared.SessionId != SessionId || prepared.LevelGeneration != _levelGeneration
                || !string.Equals(_state, "Starting", StringComparison.Ordinal)
                || (prepared.PeerId != HostPeerId && !_clients.ContainsKey(prepared.PeerId))) return;
            _sessionPreparedPeers.Add(prepared.PeerId);
            if (_sessionReleasedGeneration == _levelGeneration)
                replayRelease = true;
            else
                release = TryReleaseSessionLocked();
        }
        SessionPreparedReceived?.Invoke(this, prepared);
        if (release is not null)
            await PublishSessionReleaseAsync(release, cancellationToken).ConfigureAwait(false);
        else if (replayRelease)
        {
            var repeated = new SessionReleasePayload(SessionId, prepared.LevelGeneration);
            if (sender is null)
            {
                // A Host retry recovers a partially canceled initial broadcast
                // and re-delivers its own idempotent release notification.
                await PublishSessionReleaseAsync(repeated, cancellationToken).ConfigureAwait(false);
            }
            else
            {
                // A late Client report only releases that participant. Existing
                // participants never re-enter the loading barrier.
                await sender.Connection.SendAsync(MessageType.SessionRelease,
                    JsonPayload.Serialize(repeated), cancellationToken).ConfigureAwait(false);
            }
        }
    }

    private SessionReleasePayload? TryReleaseSessionLocked()
    {
        if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
            || _levelGeneration < 1 || _sessionReleasedGeneration == _levelGeneration
            || !_sessionPreparedPeers.Contains(HostPeerId)
            || _clients.Keys.Any(peerId => !_sessionPreparedPeers.Contains(peerId))) return null;
        _sessionReleasedGeneration = _levelGeneration;
        return new SessionReleasePayload(SessionId, _levelGeneration);
    }

    private async Task PublishSessionReleaseAsync(
        SessionReleasePayload release, CancellationToken cancellationToken)
    {
        await BroadcastAsync(MessageType.SessionRelease, JsonPayload.Serialize(release),
            cancellationToken).ConfigureAwait(false);
        lock (_stateLock)
            if (_levelGeneration != release.LevelGeneration
                || !string.Equals(_state, "Starting", StringComparison.Ordinal)) return;
        SessionReleased?.Invoke(this, release);
    }

    private async Task EvaluateSessionReleaseAfterDepartureAsync()
    {
        SessionReleasePayload? release;
        lock (_stateLock) release = TryReleaseSessionLocked();
        if (release is null) return;
        try { await PublishSessionReleaseAsync(release, _shutdown.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
    }
}
