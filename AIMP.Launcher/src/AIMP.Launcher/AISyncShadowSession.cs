using AIMP.Protocol;
using System.Buffers.Binary;
using System.Threading.Channels;

namespace AIMP.Launcher;

// Only copies observations to a separate transport. This class deliberately
// has no NativeRuntimeController and no method that can invoke CATHODE.
internal sealed class AISyncShadowSession : IDisposable
{
    private sealed record Pending(AISyncShadowBinding Binding, NativeAISyncObservation Source,
        ulong CaptureEpoch, long Tick);
    private readonly LauncherDiagnostics _diagnostics;
    private readonly Func<AISyncShadowBinding> _binding;
    private readonly Channel<Pending> _pending = Channel.CreateBounded<Pending>(
        new BoundedChannelOptions(2048) { FullMode = BoundedChannelFullMode.Wait, SingleReader = true });
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Task _worker;
    private readonly bool _enabled;
    private readonly object _eventGate = new();
    private readonly Dictionary<ulong, Guid> _boundSessions = new();
    private readonly HashSet<ulong> _ambiguousCookies = new();
    private bool _sessionCapacityExceeded;
    private LobbyHost? _eventHost, _sourceHost;
    private LobbyClient? _eventClient;
    private AISyncShadowStreamId? _stream;
    private Guid _connection;
    private ulong _captureEpoch;
    private long _sourceEpoch, _lossEpoch = -1, _lastLog;
    private long _received, _published, _rejected, _overflows;
    private int _disposed;
    internal Task Completion => _worker;
    internal long RejectedCount => Interlocked.Read(ref _rejected);

    internal AISyncShadowSession(LauncherDiagnostics diagnostics, Func<AISyncShadowBinding> binding,
        bool? enabled = null)
    {
        _diagnostics = diagnostics; _binding = binding;
        _enabled = enabled ?? (OptIn("AIMP_AISYNC_TRANSACTION_SHADOW") &&
            OptIn("AIMP_AISYNC_TRANSACTION_TRANSPORT_SHADOW"));
        _worker = _enabled ? Task.Run(RunAsync) : Task.CompletedTask;
    }
    private static bool OptIn(string key) => Environment.GetEnvironmentVariable(key) == "1";

    internal void BindSession(Guid session)
    {
        if (!_enabled || session == Guid.Empty) return;
        var cookie = AISyncCodec.SessionCookie(session);
        lock (_eventGate)
        {
            if (_boundSessions.TryGetValue(cookie, out var prior))
            { if (prior != session) _ambiguousCookies.Add(cookie); return; }
            if (_boundSessions.Count >= 128) { _sessionCapacityExceeded = true; return; }
            _boundSessions.Add(cookie, session);
        }
    }
    private bool SessionUnambiguous(Guid session)
    {
        var cookie = AISyncCodec.SessionCookie(session);
        lock (_eventGate) return !_sessionCapacityExceeded && !_ambiguousCookies.Contains(cookie) &&
            _boundSessions.TryGetValue(cookie, out var prior) && prior == session;
    }

    internal void BindHost(LobbyHost host)
    {
        lock (_eventGate)
        {
            DetachHost(); _eventHost = host;
            host.AISyncShadowReceiptReceived += OnReceipt;
            host.AISyncShadowRecoveryRequired += OnRecovery;
        }
    }
    internal void UnbindHost(LobbyHost host)
    {
        lock (_eventGate) if (ReferenceEquals(_eventHost, host)) DetachHost();
    }
    private void DetachHost()
    {
        if (_eventHost is null) return;
        _eventHost.AISyncShadowReceiptReceived -= OnReceipt;
        _eventHost.AISyncShadowRecoveryRequired -= OnRecovery;
        _eventHost = null;
    }
    internal void BindClient(LobbyClient client)
    {
        lock (_eventGate)
        {
            DetachClient(); _eventClient = client;
            client.AISyncShadowObservationBatchReceived += OnRemoteData;
            client.AISyncShadowRecoveryRequired += OnRecovery;
        }
    }
    internal void UnbindClient(LobbyClient client)
    {
        lock (_eventGate) if (ReferenceEquals(_eventClient, client)) DetachClient();
    }
    private void DetachClient()
    {
        if (_eventClient is null) return;
        _eventClient.AISyncShadowObservationBatchReceived -= OnRemoteData;
        _eventClient.AISyncShadowRecoveryRequired -= OnRecovery;
        _eventClient = null;
    }

    internal void Observe(NativeAISyncObservation observation)
    {
        if (!_enabled || Volatile.Read(ref _disposed) != 0) return;
        var binding = _binding();
        if (!ValidBinding(binding, observation)) { Reject("inactive or stale native binding"); return; }
        if (!SessionUnambiguous(binding.Session)) { Reject("ambiguous or unbound cookie; shadow fenced for this Launcher lifetime"); return; }
        try { QueueObservation(binding, observation); }
        catch (ProtocolException exception) { Reject(exception.Message); }
    }
    private static bool ValidBinding(AISyncShadowBinding binding, NativeAISyncObservation source) =>
        binding.Active && binding.Host is not null && binding.Session != Guid.Empty &&
        binding.Host.SessionId == binding.Session &&
        binding.WorldGeneration > 0 && binding.NativeGeneration > 0 &&
        source.ConnectionIdentity != Guid.Empty && source.ProcessId == binding.ProcessId;

    private void QueueObservation(AISyncShadowBinding binding, NativeAISyncObservation source)
    {
        if (source.ConnectionIdentity != binding.ConnectionIdentity) { Reject("replaced IPC connection"); return; }
        var scope = new AISyncShadowStreamId(binding.Session, (ulong)binding.WorldGeneration, source.ConnectionIdentity);
        var record = AISyncShadowCodec.ValidateNativeObservation(source.Bytes.Span, scope);
        if (record.OwnerEpoch != (ulong)binding.NativeGeneration) { Reject("stale native generation"); return; }
        var capture = BinaryPrimitives.ReadUInt64LittleEndian(source.Bytes.Span[32..]);
        var copy = source with { Bytes = source.Bytes.ToArray() };
        if (_pending.Writer.TryWrite(new(binding, copy, capture, Environment.TickCount64))) return;
        Interlocked.Increment(ref _overflows);
        Interlocked.Exchange(ref _lossEpoch, binding.Epoch);
        Reject("source queue overflow; shadow scope requires restart");
    }
    private void Reject(string reason)
    {
        Interlocked.Increment(ref _rejected);
        LogDue($"sourceRefusal={reason}");
    }

    private async Task RunAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(20));
        try { while (await timer.WaitForNextTickAsync(_shutdown.Token).ConfigureAwait(false)) await TickAsync().ConfigureAwait(false); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
        catch (Exception exception) { _diagnostics.Log("AISyncShadow", $"Source worker stopped: {exception.Message}"); }
        finally { await StopSourceAsync().ConfigureAwait(false); }
    }
    private async Task TickAsync()
    {
        var binding = _binding();
        if (_stream is not null && (!binding.Active || binding.Epoch != _sourceEpoch ||
            !ReferenceEquals(binding.Host, _sourceHost))) await StopSourceAsync().ConfigureAwait(false);
        for (var count = 0; count < 128 && _pending.Reader.TryRead(out var pending); ++count)
            await ProcessPendingAsync(pending).ConfigureAwait(false);
    }
    private async Task ProcessPendingAsync(Pending pending)
    {
        try { await PublishAsync(pending).ConfigureAwait(false); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { throw; }
        catch (Exception exception)
        {
            Reject($"source publication failed: {exception.Message}");
            await BlockSourceAsync(pending.Binding.Epoch, "publication failed").ConfigureAwait(false);
        }
    }
    private async Task PublishAsync(Pending pending)
    {
        var current = _binding();
        if (current != pending.Binding || !current.Active) { Reject("expired source context"); return; }
        if (Volatile.Read(ref _lossEpoch) == current.Epoch || Environment.TickCount64 - pending.Tick > 2000)
        { await BlockSourceAsync(current.Epoch, "source queue loss or age").ConfigureAwait(false); return; }
        await EnsureStreamAsync(pending).ConfigureAwait(false);
        if (_sourceHost is null || _stream is null) return;
        var result = await _sourceHost.PublishAISyncShadowAsync(_stream.Value,
            pending.Source.Bytes, _shutdown.Token).ConfigureAwait(false);
        if (result != AISyncShadowResult.Accepted)
        { await BlockSourceAsync(current.Epoch, $"journal refusal {result}").ConfigureAwait(false); return; }
        Interlocked.Increment(ref _published); LogDue("source=Host receivedOnly=true");
    }
    private async Task EnsureStreamAsync(Pending pending)
    {
        if (_connection == pending.Source.ConnectionIdentity && pending.CaptureEpoch < _captureEpoch)
            throw new ProtocolException("AISync shadow capture epoch regressed.");
        if (_stream is not null && _connection == pending.Source.ConnectionIdentity &&
            _captureEpoch == pending.CaptureEpoch && _sourceEpoch == pending.Binding.Epoch) return;
        await StopSourceAsync().ConfigureAwait(false);
        _sourceHost = pending.Binding.Host;
        _stream = _sourceHost!.StartAISyncShadow(pending.Binding.WorldGeneration);
        _connection = pending.Source.ConnectionIdentity;
        _captureEpoch = pending.CaptureEpoch; _sourceEpoch = pending.Binding.Epoch;
        _diagnostics.Log("AISyncShadow", $"Opened observation stream={_stream.Value.StreamGuid} " +
            $"session={pending.Binding.Session} world={pending.Binding.WorldGeneration} capture={_captureEpoch}; no replay.");
    }
    private async Task BlockSourceAsync(long epoch, string reason)
    {
        Interlocked.Exchange(ref _lossEpoch, epoch);
        if (_stream is not null) _diagnostics.Log("AISyncShadow", $"RecoveryRequired: {reason}; legacy AISync continues.");
        await StopSourceAsync().ConfigureAwait(false);
    }
    private async Task StopSourceAsync()
    {
        var host = _sourceHost;
        _sourceHost = null; _stream = null; _connection = Guid.Empty; _captureEpoch = 0;
        if (host is null) return;
        try { await host.StopAISyncShadowAsync().ConfigureAwait(false); }
        catch (Exception exception) { _diagnostics.Log("AISyncShadow", $"Shadow stop: {exception.Message}"); }
    }

    private void OnReceipt(object? sender, AISyncShadowReceipt receipt) =>
        LogDue($"receipt stream={receipt.Stream.StreamGuid} verified={receipt.VerifiedOrdinal} applied=0");
    private void OnRecovery(object? sender, AISyncShadowRecovery recovery) =>
        _diagnostics.Log("AISyncShadow", $"RecoveryRequired stream={recovery.Stream.StreamGuid} " +
            $"consumer={recovery.ConsumerGuid} reason={recovery.Reason}; no native application.");
    private void OnRemoteData(object? sender, AISyncShadowData data)
    {
        Interlocked.Add(ref _received, data.Observations.Length);
        LogDue($"role=Client stream={data.Stream.StreamGuid} verifiedDataOnly=true applied=0");
    }
    private void LogDue(string detail)
    {
        var now = Environment.TickCount64; var previous = Interlocked.Read(ref _lastLog);
        if (now - previous < 5000 || Interlocked.CompareExchange(ref _lastLog, now, previous) != previous) return;
        _diagnostics.Log("AISyncShadow", $"{detail} published={Interlocked.Read(ref _published)} " +
            $"received={Interlocked.Read(ref _received)} refused={Interlocked.Read(ref _rejected)} " +
            $"sourceOverflows={Interlocked.Read(ref _overflows)} coverageIncomplete=true");
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        lock (_eventGate) { DetachHost(); DetachClient(); }
        _pending.Writer.TryComplete(); _shutdown.Cancel();
        // Worker observes cancellation and awaits the per-peer sender shutdown.
        // Its catch/finally owns errors; no fire-and-forget packet continuations.
        _ = _worker.ContinueWith(_ => _shutdown.Dispose(), TaskScheduler.Default);
    }
}
