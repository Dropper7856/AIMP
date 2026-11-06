namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private ulong _lastAlienControlHostSequence;
    private int _lastAlienControlGeneration;
    private ulong _lastAlienControlPeer;

    public event EventHandler<AlienControlCommit>? AlienControlCommitted;

    public Task SendAlienControlAsync(AlienControlIntent intent,
        CancellationToken cancellationToken = default)
    {
        AlienControlProtocol.Validate(intent);
        if (intent.SessionId != SessionId || intent.AlienPeerId != PeerId ||
            Snapshot?.AlienVsSurvivors != true || Snapshot.AlienPeerId != PeerId)
            throw new ProtocolException("This participant is not the Alien decision maker.");
        return _connection.SendAsync(MessageType.AlienControlProposal,
            JsonPayload.Serialize(intent), cancellationToken).AsTask();
    }

    private void HandleAlienControlCommitted(ReadOnlyMemory<byte> bytes)
    {
        var value = JsonPayload.Deserialize<AlienControlCommit>(bytes.Span);
        AlienControlProtocol.Validate(value);
        var current = Snapshot;
        if (value.Intent.SessionId != SessionId ||
            current?.AlienVsSurvivors != true ||
            current.AlienPeerId != value.Intent.AlienPeerId ||
            current.LevelGeneration != value.Intent.LevelGeneration)
            throw new ProtocolException("Alien control commit does not belong to the active PvP round.");
        if (_lastAlienControlGeneration != current.LevelGeneration ||
            _lastAlienControlPeer != current.AlienPeerId)
        {
            _lastAlienControlGeneration = current.LevelGeneration;
            _lastAlienControlPeer = current.AlienPeerId;
            _lastAlienControlHostSequence = 0;
        }
        if (value.HostSequence <= _lastAlienControlHostSequence) return;
        _lastAlienControlHostSequence = value.HostSequence;
        AlienControlCommitted?.Invoke(this, value);
    }
}
