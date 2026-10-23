namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    public event EventHandler<ReadOnlyMemory<byte>>? ThrowableProposed;
    private readonly Dictionary<ulong, ulong> _throwableProposals = new();
    private readonly Dictionary<ulong, ulong> _throwableCommits = new();
    private readonly HashSet<(ulong Peer, ulong Sequence)> _liveThrowables = new();
    private int _throwableGeneration;

    private bool ThrowableContextLocked(ReadOnlySpan<byte> bytes)
    {
        if (_state != "Starting" || ThrowableSyncCodec.Generation(bytes) != _levelGeneration
            || ThrowableSyncCodec.Cookie(bytes) != AISyncCodec.SessionCookie(SessionId)) return false;
        if (_throwableGeneration != _levelGeneration)
        {
            _throwableGeneration = _levelGeneration;
            _throwableProposals.Clear(); _throwableCommits.Clear(); _liveThrowables.Clear();
        }
        return true;
    }

    private void HandleThrowableProposal(ClientState state, ReadOnlyMemory<byte> payload)
    {
        ThrowableSyncCodec.Validate(payload.Span);
        var peer = ThrowableSyncCodec.Peer(payload.Span);
        var sequence = ThrowableSyncCodec.Sequence(payload.Span);
        if (peer != state.PeerId || ThrowableSyncCodec.Operation(payload.Span) != 1)
            throw new ProtocolException("A throwable proposal must be the authenticated sender's release.");
        lock (_stateLock)
        {
            if (!ThrowableContextLocked(payload.Span)
                || _throwableProposals.TryGetValue(peer, out var last) && sequence <= last) return;
            _throwableProposals[peer] = sequence;
        }
        // Native Host admission must succeed before any Client sees a commit.
        ThrowableProposed?.Invoke(this, payload);
    }

    public async Task CommitThrowableAsync(ReadOnlyMemory<byte> payload, CancellationToken token = default)
    {
        ThrowableSyncCodec.Validate(payload.Span);
        var peer = ThrowableSyncCodec.Peer(payload.Span);
        var sequence = ThrowableSyncCodec.Sequence(payload.Span);
        var operation = ThrowableSyncCodec.Operation(payload.Span);
        lock (_stateLock)
        {
            if (!ThrowableContextLocked(payload.Span)) return;
            if (operation == 2)
            {
                if (!_liveThrowables.Remove((peer, sequence))) return;
            }
            else
            {
                if (_liveThrowables.Count >= 128
                    || _throwableCommits.TryGetValue(peer, out var last) && sequence <= last) return;
                if (peer != HostPeerId && (!_clients.ContainsKey(peer)
                    || !_throwableProposals.TryGetValue(peer, out var proposed) || sequence > proposed)) return;
                _throwableCommits[peer] = sequence;
                _liveThrowables.Add((peer, sequence));
            }
        }
        await BroadcastAsync(MessageType.ThrowableCommit, payload.ToArray(), token).ConfigureAwait(false);
    }
}
