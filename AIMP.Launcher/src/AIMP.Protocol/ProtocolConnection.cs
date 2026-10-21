using System.Net.Sockets;

namespace AIMP.Protocol;

internal sealed class ProtocolConnection : IAsyncDisposable
{
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private long _sequence;
    private static readonly TimeSpan VoiceWriteWait = TimeSpan.FromMilliseconds(10);

    public ProtocolConnection(TcpClient client)
    {
        Client = client;
        Client.NoDelay = true;
        Stream = client.GetStream();
    }

    public TcpClient Client { get; }

    public NetworkStream Stream { get; }

    public async ValueTask SendAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        await _writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref _sequence));
            await FrameCodec.WriteAsync(
                Stream,
                new AimpFrame(type, 0, sequence, payload),
                cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _writeLock.Release();
        }
    }

    public ValueTask<AimpFrame> ReadAsync(CancellationToken cancellationToken) =>
        FrameCodec.ReadAsync(Stream, cancellationToken);

    // Voice never queues behind other writes. A congested connection drops
    // the frame; reliable gameplay/control messages keep their existing path.
    public async Task<bool> TrySendVoiceAsync(ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        // A short bounded wait rides out position/AI writes; a congested link still drops.
        if (!await _writeLock.WaitAsync(VoiceWriteWait, token).ConfigureAwait(false)) return false;
        try
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref _sequence));
            await FrameCodec.WriteAsync(Stream,
                new AimpFrame(MessageType.VoiceFrame, 0, sequence, payload), token).ConfigureAwait(false);
            return true;
        }
        finally { _writeLock.Release(); }
    }

    public ValueTask DisposeAsync()
    {
        Client.Dispose();
        _writeLock.Dispose();
        return ValueTask.CompletedTask;
    }
}
