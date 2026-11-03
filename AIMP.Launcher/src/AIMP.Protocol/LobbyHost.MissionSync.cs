namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private readonly SemaphoreSlim _missionCommitGate = new(1, 1);
    private readonly Dictionary<ulong, ulong> _missionProposals = new();
    // A mission frontier is a progression decision: commit it once per round.
    // Every participant can raise the same one (each player enters a trigger
    // box, or a delayed consequence fires on every machine); the duplicate must
    // not replay the authored branch twice nor consume a commit sequence.
    private readonly HashSet<(string EntityId, uint Signal, byte Selector)> _missionCommittedFrontiers = new();
    // A repeatable frontier (a hub terminal starting a nested mission) is only
    // raised by the participant who used it, possibly several times a round.
    // A short window still collapses near-simultaneous duplicate proposals.
    private static readonly TimeSpan DefaultRepeatableMissionWindow = TimeSpan.FromSeconds(10);
    private long _missionRepeatableWindowMilliseconds = (long)DefaultRepeatableMissionWindow.TotalMilliseconds;
    private readonly HashSet<(string EntityId, uint Signal)> _missionRepeatable = new();
    private readonly Dictionary<(string EntityId, uint Signal, byte Selector), long> _missionRepeatableCommits = new();
    private int _missionGeneration;
    private ulong _missionCommitSequence;
    private long _missionDrops;

    public event EventHandler<MissionSignalPayload>? MissionSignalCommitted;

    public void SetMissionRepeatableFrontiers(IEnumerable<(string EntityId, uint Signal)> frontiers,
        TimeSpan? duplicateWindow = null)
    {
        ArgumentNullException.ThrowIfNull(frontiers);
        var window = duplicateWindow ?? DefaultRepeatableMissionWindow;
        if (window < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duplicateWindow));
        lock (_stateLock)
        {
            _missionRepeatableWindowMilliseconds = (long)window.TotalMilliseconds;
            _missionRepeatable.Clear();
            foreach (var (entityId, signal) in frontiers)
                _missionRepeatable.Add((entityId.ToUpperInvariant(), signal));
            _missionRepeatableCommits.Clear();
        }
    }

    public Task ReportHostMissionSignalAsync(MissionSignalPayload signal,
        CancellationToken cancellationToken = default)
    {
        MissionSignalValidation.Validate(signal);
        if (signal.SessionId != SessionId || signal.PeerId != HostPeerId)
            throw new InvalidOperationException("The Host mission source identity is invalid.");
        return CommitMissionSignalAsync(signal, null, cancellationToken);
    }

    private Task HandleMissionSignalAsync(ClientState sender, ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var signal = JsonPayload.Deserialize<MissionSignalPayload>(bytes.Span);
        MissionSignalValidation.Validate(signal);
        return CommitMissionSignalAsync(signal, sender, cancellationToken);
    }

    private async Task CommitMissionSignalAsync(MissionSignalPayload proposal, ClientState? sender,
        CancellationToken cancellationToken)
    {
        await _missionCommitGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            MissionSignalPayload? committed = null;
            string? rejection = null;
            bool capacityExceeded = false;
            lock (_stateLock)
            {
                if (proposal.SessionId != SessionId || proposal.LevelGeneration != _levelGeneration
                    || _state != "Starting" || (sender is not null
                        && (sender.PeerId != proposal.PeerId
                            || !_clients.TryGetValue(sender.PeerId, out var current)
                            || !ReferenceEquals(current, sender))))
                    rejection = "source or active round rejected";
                else
                {
                    // Reset only after validating the new active generation: stale packets
                    // must never roll back replay protection for the current round.
                    if (_missionGeneration != _levelGeneration)
                    {
                        _missionGeneration = _levelGeneration;
                        _missionCommitSequence = 0;
                        _missionProposals.Clear();
                        _missionCommittedFrontiers.Clear();
                        _missionRepeatableCommits.Clear();
                    }
                    if (_missionProposals.TryGetValue(proposal.PeerId, out var previous)
                        && proposal.Sequence <= previous)
                        rejection = "replayed proposal rejected";
                    else if (_missionCommitSequence >= MissionSignalValidation.MaximumSignalsPerGeneration)
                        capacityExceeded = true;
                    else
                    {
                        _missionProposals[proposal.PeerId] = proposal.Sequence;
                        var frontier = (proposal.EntityId.ToUpperInvariant(), proposal.Signal, proposal.InterfaceSelector);
                        if (_missionRepeatable.Contains((frontier.Item1, frontier.Item2)))
                        {
                            var now = Environment.TickCount64;
                            if (!_missionRepeatableCommits.TryGetValue(frontier, out var last)
                                || now - last >= _missionRepeatableWindowMilliseconds)
                            {
                                _missionRepeatableCommits[frontier] = now;
                                committed = proposal with { Sequence = ++_missionCommitSequence };
                            }
                            else rejection = "repeatable mission frontier already committed moments ago";
                        }
                        else if (_missionCommittedFrontiers.Add(frontier))
                            committed = proposal with { Sequence = ++_missionCommitSequence };
                        else rejection = "mission frontier already committed this round";
                    }
                }
            }
            if (capacityExceeded)
            {
                const string message = "MissionSync reached its 4096-signal round limit; mission progress cannot continue safely.";
                MissionSignalDropped(message);
                if (sender is null) throw new ProtocolException(message);
                await SendErrorAsync(sender.Connection, "MISSION_SYNC_CAPACITY", message, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }
            if (committed is null)
            {
                MissionSignalDropped(rejection!);
                return;
            }
            // Every participant, the Host and the proposer included, suppressed its
            // local frontier and executes the authored branch only from this commit.
            MissionSignalCommitted?.Invoke(this, committed);
            await BroadcastAsync(MessageType.MissionSignalCommitted, JsonPayload.Serialize(committed),
                _shutdown.Token).ConfigureAwait(false);
        }
        finally { _missionCommitGate.Release(); }
    }

    private void MissionSignalDropped(string reason)
    {
        var count = Interlocked.Increment(ref _missionDrops);
        if ((count & (count - 1)) == 0)
            Log?.Invoke(this, $"MissionSync dropped ({count}): {reason}.");
    }
}
