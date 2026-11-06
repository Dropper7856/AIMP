namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private ulong _lastAlienControlSequence;
    private ulong _alienCommittedSequence;
    private readonly SemaphoreSlim _alienControlDispatchGate = new(1, 1);

    // Validated on the Host, but the *source of decisions* remains AlienPeerId.
    public event EventHandler<AlienControlCommit>? AlienControlCommitted;

    public Task SubmitHostAlienControlAsync(AlienControlIntent intent,
        CancellationToken cancellationToken = default) =>
        AcceptAlienControlAsync(intent, HostPeerId, cancellationToken);

    private async Task HandleAlienControlProposalAsync(ClientState client,
        ReadOnlyMemory<byte> bytes, CancellationToken token)
    {
        var intent = JsonPayload.Deserialize<AlienControlIntent>(bytes.Span);
        AlienControlProtocol.Validate(intent);
        if (intent.AlienPeerId != client.PeerId)
            throw new ProtocolException("Alien control cannot spoof another participant.");
        await AcceptAlienControlAsync(intent, client.PeerId, token).ConfigureAwait(false);
    }

    private async Task AcceptAlienControlAsync(AlienControlIntent intent, ulong sender,
        CancellationToken cancellationToken)
    {
        AlienControlProtocol.Validate(intent);
        // Admission and publication must preserve one ordered stream.
        await _alienControlDispatchGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
        AlienControlCommit? committed = null;
        lock (_stateLock)
        {
            if (!_options.AlienVsSurvivors || _state != "Starting" ||
                _alienPeerId == 0 || _alienPeerId != sender ||
                intent.AlienPeerId != sender || intent.SessionId != SessionId ||
                intent.LevelGeneration != _levelGeneration)
                throw new ProtocolException("Alien control was not submitted by the active Alien.");

            // Input samples are strictly monotonic; no repeated vent or attack
            // edges after a TCP reconnect/retransmission.
            if (intent.Sequence <= _lastAlienControlSequence) return;
            // A consumed sequence is never re-opened if a lower sample arrives.
            _lastAlienControlSequence = intent.Sequence;
            if (_alienCommittedSequence == ulong.MaxValue)
                throw new ProtocolException("Alien command sequence exhausted.");
            committed = new AlienControlCommit(++_alienCommittedSequence, intent);
        }
        AlienControlProtocol.Validate(committed);
        // A lost broadcast is not replayed without a new, higher input sample.
        // Combat still requires a native host-authoritative result.
        await BroadcastAsync(MessageType.AlienControlCommitted,
            JsonPayload.Serialize(committed), cancellationToken).ConfigureAwait(false);
        AlienControlCommitted?.Invoke(this, committed);
        }
        finally { _alienControlDispatchGate.Release(); }
    }
}
