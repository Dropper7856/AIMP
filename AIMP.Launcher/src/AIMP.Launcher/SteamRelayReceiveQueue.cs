using System.Threading.Channels;

namespace AIMP.Launcher;

// At most 8 MiB queued per peer, plus the message currently being written to TCP.
// Reliable stream bytes cannot be dropped: the caller must close on overflow.
internal sealed class SteamRelayReceiveQueue
{
    internal const int Capacity = 128;
    internal const int MaximumMessageBytes = 64 * 1024;
    private readonly Channel<byte[]> _messages = Channel.CreateBounded<byte[]>(
        new BoundedChannelOptions(Capacity)
        {
            SingleReader = true,
            SingleWriter = true,
            FullMode = BoundedChannelFullMode.Wait
        });

    internal bool TryWrite(byte[] bytes) => bytes.Length is > 0 and <= MaximumMessageBytes
        && _messages.Writer.TryWrite(bytes);
    internal IAsyncEnumerable<byte[]> ReadAllAsync(CancellationToken token) => _messages.Reader.ReadAllAsync(token);
    internal void Complete() => _messages.Writer.TryComplete();
}
