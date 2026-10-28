namespace AIMP.Protocol;

// A single bounded pump per peer/direction; publishing never awaits a socket.
internal sealed class AISyncExecutionSender : IAsyncDisposable
{
    private sealed record Pending(MessageType Type, byte[] Bytes, long Tick, bool Terminal);
    private readonly object _gate = new();
    private readonly Queue<Pending> _queue = new();
    private readonly SemaphoreSlim _signal = new(0, 1);
    private readonly CancellationTokenSource _stop;
    private readonly Func<MessageType, ReadOnlyMemory<byte>, CancellationToken, Task> _send;
    private readonly Action _failure;
    private int _bytes, _inFlight;
    private bool _terminal;
    private int _disposed;
    internal Task Completion { get; }
    internal AISyncExecutionSender(Func<MessageType, ReadOnlyMemory<byte>, CancellationToken, Task> send,
        Action failure, CancellationToken shutdown)
    {
        _send = send; _failure = failure;
        _stop = CancellationTokenSource.CreateLinkedTokenSource(shutdown);
        Completion = Task.Run(RunAsync);
    }
    internal bool Enqueue(MessageType type, ReadOnlySpan<byte> bytes)
    {
        lock (_gate)
        {
            if (_terminal || _disposed != 0 || _queue.Count >= 2048 || bytes.Length > 2 * 1024 * 1024 - _bytes - _inFlight)
                return false;
            _queue.Enqueue(new(type, bytes.ToArray(), Environment.TickCount64, false)); _bytes += bytes.Length;
            Signal(); return true;
        }
    }
    internal void Finish(AISyncExecutionControl terminal)
    {
        var bytes = AISyncExecutionControlCodec.Encode(terminal);
        lock (_gate)
        {
            if (_terminal || _disposed != 0) return;
            _terminal = true; _queue.Clear(); _bytes = bytes.Length;
            _queue.Enqueue(new(MessageType.AISyncExecutionControl, bytes, Environment.TickCount64, true)); Signal();
        }
    }
    private void Signal() { if (_signal.CurrentCount == 0) _signal.Release(); }
    private async Task RunAsync()
    {
        try
        {
            while (true)
            {
                await _signal.WaitAsync(_stop.Token).ConfigureAwait(false);
                Pending? value;
                lock (_gate)
                {
                    value = _queue.Count == 0 ? null : _queue.Dequeue();
                    if (value is not null) { _bytes -= value.Bytes.Length; _inFlight = value.Bytes.Length; }
                }
                if (value is null) continue;
                if (Environment.TickCount64 - value.Tick > 2000) throw new IOException("AISync execution queue expired.");
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token);
                timeout.CancelAfter(TimeSpan.FromSeconds(2));
                await _send(value.Type, value.Bytes, timeout.Token).ConfigureAwait(false);
                lock (_gate) { _inFlight = 0; if (_queue.Count != 0) Signal(); }
                if (value.Terminal) return;
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        catch (Exception) { try { _failure(); } catch (Exception) { } }
        finally { lock (_gate) { _terminal = true; _queue.Clear(); _bytes = _inFlight = 0; } }
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _stop.Cancel(); await Completion.ConfigureAwait(false);
        _stop.Dispose(); _signal.Dispose();
    }
}
