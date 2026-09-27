using System.Net.Sockets;

namespace AIMP.Protocol;

internal sealed class ProtocolConnection : IAsyncDisposable
{
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private long _sequence;

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

    public ValueTask DisposeAsync()
    {
        Client.Dispose();
        _writeLock.Dispose();
        return ValueTask.CompletedTask;
    }
}
