namespace AIMP.Protocol;

// Mission completion: a participant that reached a retail level exit is
// finished, not dead. The round ends once every participant is dead or
// finished; it is a victory when at least one participant finished.
public sealed partial class LobbyHost
{
    private readonly HashSet<ulong> _finishedPeers = new();

    public event EventHandler<PlayerMissionCompletePayload>? PlayerMissionCompleteReceived;

    public event EventHandler<RoundProgressPayload>? RoundProgressChanged;

    public Task ReportHostMissionCompleteAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = new PlayerMissionCompletePayload(SessionId, HostPeerId, levelGeneration);
        ModelValidation.Validate(payload);
        return RecordMissionCompleteAsync(payload, cancellationToken);
    }

    private async Task RecordMissionCompleteAsync(
        PlayerMissionCompletePayload payload,
        CancellationToken cancellationToken)
    {
        RoundEndedPayload? ended;
        RoundProgressPayload? progress;
        lock (_stateLock)
        {
            // A dead participant cannot finish afterwards (no respawn).
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
                || payload.LevelGeneration != _levelGeneration
                || _deadPeers.Contains(payload.PeerId)
                || (_options.AlienVsSurvivors && payload.PeerId == _alienPeerId)
                || !_finishedPeers.Add(payload.PeerId)) return;
            progress = CreateRoundProgressLocked();
            ended = TryEndRoundLocked();
        }

        Log?.Invoke(this, $"Participant {payload.PeerId} completed the mission in generation {payload.LevelGeneration}.");
        PlayerMissionCompleteReceived?.Invoke(this, payload);
        await PublishRoundOutcomeAsync(progress, ended, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandlePlayerMissionCompleteAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<PlayerMissionCompletePayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != state.PeerId)
        {
            await SendErrorAsync(state.Connection, "INVALID_MISSION_COMPLETE",
                "Player mission-complete identity is invalid.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        // A stale generation is ignored without error, like PlayerDeath.
        await RecordMissionCompleteAsync(payload, cancellationToken).ConfigureAwait(false);
    }

    // Caller holds _stateLock.
    private RoundProgressPayload CreateRoundProgressLocked()
    {
        var participants = _clients.Keys.Append(HostPeerId).ToHashSet();
        var payload = new RoundProgressPayload(SessionId, _levelGeneration,
            _finishedPeers.Count(participants.Contains), _deadPeers.Count(participants.Contains),
            participants.Count);
        ModelValidation.Validate(payload);
        return payload;
    }

    // Caller holds _stateLock.
    private bool RoundTerminalLocked(ulong peerId) =>
        _deadPeers.Contains(peerId) || _finishedPeers.Contains(peerId);

    // Caller holds _stateLock.
    private string RoundEndReasonLocked() =>
        _finishedPeers.Count != 0 ? "MissionComplete" : "AllPlayersDead";

    // Progress is informational; only the RoundEnded commit changes state.
    private async Task PublishRoundOutcomeAsync(
        RoundProgressPayload? progress,
        RoundEndedPayload? ended,
        CancellationToken cancellationToken)
    {
        if (ended is not null)
        {
            await PublishRoundEndedAsync(ended, cancellationToken).ConfigureAwait(false);
            return;
        }
        if (progress is null) return;
        await BroadcastAsync(MessageType.RoundProgress, JsonPayload.Serialize(progress),
            cancellationToken).ConfigureAwait(false);
        RoundProgressChanged?.Invoke(this, progress);
    }
}
