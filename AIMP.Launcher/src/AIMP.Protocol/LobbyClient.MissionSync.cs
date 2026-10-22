namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private readonly SemaphoreSlim _missionProposalGate = new(1, 1);
    private int _missionSendGeneration;
    private ulong _missionLastProposal;
    // Only the ordered connection read loop touches incoming commit state.
    private int _missionReceiveGeneration;
    private ulong _missionLastCommit;
    private long _missionDrops;

    public event EventHandler<MissionSignalPayload>? MissionSignalCommitted;

    public async Task SendMissionSignalAsync(MissionSignalPayload signal,
        CancellationToken cancellationToken = default)
    {
        MissionSignalValidation.Validate(signal);
        if (signal.SessionId != SessionId || signal.PeerId != PeerId)
            throw new InvalidOperationException("The client mission source identity is invalid.");
        await _missionProposalGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var snapshot = Snapshot;
            if (snapshot?.State != "Starting" || snapshot.LevelGeneration != signal.LevelGeneration)
            {
                MissionSignalDropped("outbound round rejected");
                return;
            }
            if (_missionSendGeneration != signal.LevelGeneration)
            {
                _missionSendGeneration = signal.LevelGeneration;
                _missionLastProposal = 0;
            }
            if (signal.Sequence <= _missionLastProposal)
            {
                MissionSignalDropped("outbound proposal replay rejected");
                return;
            }
            await _connection.SendAsync(MessageType.MissionSignalProposal, JsonPayload.Serialize(signal),
                cancellationToken).ConfigureAwait(false);
            _missionLastProposal = signal.Sequence;
        }
        finally { _missionProposalGate.Release(); }
    }

    private void HandleMissionSignalCommitted(ReadOnlyMemory<byte> bytes)
    {
        var signal = JsonPayload.Deserialize<MissionSignalPayload>(bytes.Span);
        MissionSignalValidation.Validate(signal);
        var snapshot = Snapshot;
        if (signal.SessionId != SessionId || snapshot?.State != "Starting"
            || snapshot.LevelGeneration != signal.LevelGeneration)
        {
            MissionSignalDropped("committed round rejected");
            return;
        }
        if (_missionReceiveGeneration != signal.LevelGeneration)
        {
            _missionReceiveGeneration = signal.LevelGeneration;
            _missionLastCommit = 0;
        }
        if (signal.Sequence <= _missionLastCommit)
        {
            MissionSignalDropped("committed replay rejected");
            return;
        }
        if (signal.Sequence != _missionLastCommit + 1
            || signal.Sequence > MissionSignalValidation.MaximumSignalsPerGeneration)
            throw new ProtocolException("MissionSync commit order is incomplete or exceeds the round limit.");
        // The source can have left after the commit was sent. The Host connection
        // is the authority; do not reject a valid commit using a newer peer list.
        _missionLastCommit = signal.Sequence;
        MissionSignalCommitted?.Invoke(this, signal);
    }

    private void MissionSignalDropped(string reason)
    {
        var count = Interlocked.Increment(ref _missionDrops);
        if ((count & (count - 1)) == 0)
            Log?.Invoke(this, $"MissionSync dropped ({count}): {reason}.");
    }
}
