using System.Net.Sockets;

namespace AIMP.Protocol;

internal sealed class ProtocolConnection : IAsyncDisposable
{
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly CancellationTokenSource _closed = new();
    private readonly CancellationToken _closedToken;
    private int _disposed;
    private long _sequence;
    private static readonly TimeSpan VoiceWriteWait = TimeSpan.FromMilliseconds(10);

    public ProtocolConnection(TcpClient client)
    {
        Client = client;
        Client.NoDelay = true;
        Stream = client.GetStream();
        _closedToken = _closed.Token;
    }

    public TcpClient Client { get; }

    public NetworkStream Stream { get; }

    public async ValueTask SendAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        await AcquireWriteAsync(cancellationToken, Timeout.InfiniteTimeSpan).ConfigureAwait(false);
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

    // A timed-out wait has written nothing. Once a frame write starts, failure
    // may leave a partial header/payload: close this peer rather than append a
    // gameplay frame into a corrupt byte stream. Other peers remain independent.
    internal async Task SendShadowAsync(MessageType type, ReadOnlyMemory<byte> payload,
        CancellationToken token)
    {
        if (type is not (MessageType.AISyncShadowOpen or MessageType.AISyncShadowData
            or MessageType.AISyncShadowReceipt or MessageType.AISyncShadowRecovery))
            throw new ArgumentOutOfRangeException(nameof(type));
        await AcquireWriteAsync(token, Timeout.InfiniteTimeSpan).ConfigureAwait(false);
        try
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref _sequence));
            await FrameCodec.WriteAsync(Stream, new AimpFrame(type, 0, sequence, payload), token).ConfigureAwait(false);
        }
        catch { Client.Dispose(); throw; }
        finally { _writeLock.Release(); }
    }

    internal async Task SendExecutionAsync(MessageType type, ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        if (type is not (MessageType.AISyncExecutionControl or MessageType.AISyncExecutionNetworkBatch
            or MessageType.AISyncExecutionNetworkReceipt or MessageType.AISyncExecutionNetworkRetry
            or MessageType.AISyncRouteNetwork or MessageType.AISyncCausalStateNetwork)) throw new ArgumentOutOfRangeException(nameof(type));
        await AcquireWriteAsync(token, Timeout.InfiniteTimeSpan).ConfigureAwait(false);
        try
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref _sequence));
            await FrameCodec.WriteAsync(Stream, new AimpFrame(type, 0, sequence, payload), token).ConfigureAwait(false);
        }
        catch { Client.Dispose(); throw; }
        finally { _writeLock.Release(); }
    }

    // Voice never queues behind other writes. A congested connection drops
    // the frame; reliable gameplay/control messages keep their existing path.
    public async Task<bool> TrySendVoiceAsync(ReadOnlyMemory<byte> payload, CancellationToken token)
    {
        // A short bounded wait rides out position/AI writes; a congested link still drops.
        if (!await AcquireWriteAsync(token, VoiceWriteWait).ConfigureAwait(false)) return false;
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
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return ValueTask.CompletedTask;
        _closed.Cancel();
        Client.Dispose();
        _closed.Dispose();
        // SemaphoreSlim.Dispose does not complete pending waiters. Cancel all
        // connection waits first and let GC collect this managed-only semaphore;
        // a writer already holding it must still be able to release in finally.
        return ValueTask.CompletedTask;
    }
    private async Task<bool> AcquireWriteAsync(CancellationToken token, TimeSpan wait)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(token, _closedToken);
        try { return await _writeLock.WaitAsync(wait, linked.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (_closedToken.IsCancellationRequested && !token.IsCancellationRequested)
        { throw new IOException("The protocol connection closed while waiting to write."); }
    }
}
