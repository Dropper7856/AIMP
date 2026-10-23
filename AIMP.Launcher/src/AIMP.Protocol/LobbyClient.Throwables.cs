namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    public event EventHandler<ReadOnlyMemory<byte>>? ThrowableCommitted;

    public Task SendThrowableProposalAsync(ReadOnlyMemory<byte> payload, CancellationToken token = default)
    {
        ThrowableSyncCodec.Validate(payload.Span);
        if (ThrowableSyncCodec.Peer(payload.Span) != PeerId || ThrowableSyncCodec.Operation(payload.Span) != 1
            || ThrowableSyncCodec.Cookie(payload.Span) != AISyncCodec.SessionCookie(SessionId))
            throw new ProtocolException("Invalid local throwable owner/session.");
        return _connection.SendAsync(MessageType.ThrowableProposal, payload, token).AsTask();
    }

    private void HandleThrowableCommit(ReadOnlyMemory<byte> payload)
    {
        ThrowableSyncCodec.Validate(payload.Span);
        if (ThrowableSyncCodec.Cookie(payload.Span) != AISyncCodec.SessionCookie(SessionId))
            throw new ProtocolException("Invalid throwable commit session.");
        ThrowableCommitted?.Invoke(this, payload);
    }
}
