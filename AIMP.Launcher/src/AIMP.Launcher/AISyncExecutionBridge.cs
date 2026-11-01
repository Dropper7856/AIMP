using AIMP.Protocol;
using System.Threading.Channels;

namespace AIMP.Launcher;

internal sealed record AISyncExecutionBinding(LobbyClient? Client, Guid Session, int WorldGeneration,
    int NativeGeneration, int ProcessId, Guid ConnectionIdentity, long Epoch, bool Active);

// Client bridge only. No shadow promotion, native receipt fabrication, Host
// publisher or actor-ownership grant exists in this transport component.
internal sealed class AISyncExecutionBridge : IDisposable
{
    internal delegate Task NativeSend(MessageType type, Guid connection, int processId,
        ReadOnlyMemory<byte> bytes, CancellationToken token);
    private enum WorkKind { Control, Batch, Retry, Route, State, Native }
    private sealed record Work(WorkKind Kind, AISyncExecutionBinding Binding, byte[] Bytes,
        NativeAISyncExecution? Native, long Tick);
    private readonly Func<AISyncExecutionBinding> _capture;
    private readonly NativeSend _send;
    private readonly Action<string> _log;
    private readonly bool _enabled;
    private readonly Channel<Work> _queue = Channel.CreateBounded<Work>(new BoundedChannelOptions(128)
        { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
    private readonly CancellationTokenSource _shutdown = new();
    private LobbyClient? _client;
    private AISyncExecutionBinding? _nativeBinding;
    private AISyncExecutionScope? _scope;
    private uint _profiles;
    private ulong _routePackets, _statePackets;
    // Diagnostics only: bounded, bridge-worker-owned values; never admission state.
    private readonly Dictionary<ulong,long> _diagnosticSent = new();
    private long _diagnosticTick, _maxQueueAge, _maxIpcSend, _maxReceiptAge;
    private ulong _diagnosticWork, _diagnosticWaiting, _diagnosticAdmitted, _diagnosticDropped;
    private bool _waitingReady;
    // A network Offer can arrive while the new round is still configuring
    // native AISync. Preserve this exact scope instead of silently dropping
    // its work item when the local binding epoch changes during startup.
    private Work? _deferredOffer;
    private const long ReadyTimeoutMilliseconds = 12000;
    private const long OfferDeferMilliseconds = 15000;
    // Native operations waiting on a local dependency (actor order, resources,
    // reservation). Retried here on the next bridge tick, in each actor's
    // order, instead of waiting one Host network round trip per operation:
    // that round trip drained a backlog at ~55 ms per operation and made the
    // Client Alien start/stop moves far behind the Host.
    private readonly Dictionary<ulong, (AISyncExecutionReceipt Receipt, long Tick)> _localWaiting = new();
    private ulong _localRetries;
    private long _openedAt, _lossEpoch = long.MinValue;
    private int _disposed;
    internal Task Completion { get; }

    internal AISyncExecutionBridge(Func<AISyncExecutionBinding> capture, NativeSend send,
        Action<string> log, bool? enabled = null)
    {
        _capture = capture; _send = send; _log = log;
        _enabled = enabled ?? true;
        Completion = _enabled ? Task.Run(RunAsync) : Task.CompletedTask;
    }
    internal void BindClient(LobbyClient client)
    {
        DetachClient(); _client = client;
        client.AISyncExecutionControlReceived += OnControl;
        client.AISyncExecutionBatchReceived += OnBatch;
        client.AISyncExecutionRetryReceived += OnRetry;
        client.AISyncRouteReceived += OnRoute;
        client.AISyncCausalStateReceived += OnState;
    }
    internal void UnbindClient(LobbyClient client)
    { if (ReferenceEquals(_client, client)) DetachClient(); }
    private void DetachClient()
    {
        var client = Interlocked.Exchange(ref _client, null);
        if (client is null) return;
        client.AISyncExecutionControlReceived -= OnControl;
        client.AISyncExecutionBatchReceived -= OnBatch;
        client.AISyncExecutionRetryReceived -= OnRetry;
        client.AISyncRouteReceived -= OnRoute;
        client.AISyncCausalStateReceived -= OnState;
    }
    private void OnControl(object? sender, AISyncExecutionControl control)
    {
        if (sender is not LobbyClient client || !ReferenceEquals(client, _client)) return;
        if (!_enabled) {
            if (control.Kind == AISyncExecutionControlKind.Offer)
                client.RejectAISyncExecution(control.Scope, AISyncExecutionRecovery.NativeUnavailable);
            return;
        }
        Queue(WorkKind.Control, AISyncExecutionControlCodec.Encode(control));
    }
    private void OnBatch(object? sender, AISyncExecutionBatch batch)
    { if (ReferenceEquals(sender, _client)) Queue(WorkKind.Batch, AISyncExecutionCodec.EncodeBatch(batch)); }
    private void OnRetry(object? sender, AISyncExecutionBatch batch)
    { if (ReferenceEquals(sender, _client)) Queue(WorkKind.Retry, AISyncExecutionCodec.EncodeBatch(batch)); }
    private void OnRoute(object? sender, AISyncRouteGraph graph)
    { if (ReferenceEquals(sender, _client)) Queue(WorkKind.Route, AISyncRouteCodec.Encode(graph)); }
    private void OnState(object? sender, AISyncCausalState value)
    { if (ReferenceEquals(sender, _client)) Queue(WorkKind.State, AISyncCausalStateCodec.Encode(value)); }
    internal void ObserveNative(NativeAISyncExecution packet)
    {
        if (!_enabled || Volatile.Read(ref _disposed) != 0) return;
        var binding = _capture();
        if (packet.ConnectionIdentity != binding.ConnectionIdentity || packet.ProcessId != binding.ProcessId) return;
        Queue(WorkKind.Native, packet.Bytes.ToArray(), packet);
    }
    private void Queue(WorkKind kind, byte[] bytes, NativeAISyncExecution? native = null)
    {
        if (!_enabled || Volatile.Read(ref _disposed) != 0) return;
        var binding = _capture();
        if (_queue.Writer.TryWrite(new(kind, binding, bytes, native, Environment.TickCount64))) return;
        Interlocked.Exchange(ref _lossEpoch, binding.Epoch);
        _log("Execution bridge overflow; current native binding requires recovery.");
    }
    private async Task RunAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(20));
        try { while (await timer.WaitForNextTickAsync(_shutdown.Token).ConfigureAwait(false)) await TickAsync().ConfigureAwait(false); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
        catch (Exception exception) { _log($"Execution bridge stopped: {exception.Message}"); }
        finally { await RetireAsync(AISyncExecutionRecovery.TransportLost).ConfigureAwait(false); }
    }
    private async Task TickAsync()
    {
        LogLatency();
        await FlushDeferredOfferAsync().ConfigureAwait(false);
        if (_nativeBinding is not null && (_capture() != _nativeBinding
            || Volatile.Read(ref _lossEpoch) == _nativeBinding.Epoch
            || (_waitingReady && Environment.TickCount64 - _openedAt > ReadyTimeoutMilliseconds)))
        {
            if (_waitingReady && Environment.TickCount64 - _openedAt > ReadyTimeoutMilliseconds)
                _log($"Native execution Ready timeout: round={_nativeBinding.WorldGeneration}, native={_nativeBinding.NativeGeneration}, authority={_scope?.AuthorityEpoch}.");
            await RetireAsync(AISyncExecutionRecovery.RestartRequired).ConfigureAwait(false);
        }
        for (var n = 0; n < 32 && _queue.Reader.TryRead(out var work); n++)
        {
            try { await ProcessAsync(work).ConfigureAwait(false); }
            catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { throw; }
            catch (Exception exception) { _log($"Execution bridge refused: {exception.Message}"); await RefuseAsync(work).ConfigureAwait(false); }
        }
        await RetryLocalAsync().ConfigureAwait(false);
    }
    private async Task RetryLocalAsync()
    {
        var binding = _nativeBinding;
        if (binding is null || _scope is null || _waitingReady || _localWaiting.Count == 0 || !BindingCurrent(binding)) return;
        var now = Environment.TickCount64; var sent = 0;
        var actors = _localWaiting.Values
            .Where(w => w.Receipt.Scope == _scope)
            .GroupBy(w => (w.Receipt.Operation.EntityA, w.Receipt.Operation.EntityB, w.Receipt.Operation.Birth))
            .ToArray();
        foreach (var actor in actors)
            foreach (var waiting in actor.OrderBy(w => w.Receipt.Operation.GlobalSequence).Take(8))
            {
                // Native answers each retry with a fresh receipt; leave it a tick.
                if (sent >= 64 || now - waiting.Tick < 15) break;
                var receipt = waiting.Receipt;
                _localWaiting[receipt.Operation.GlobalSequence] = (receipt, now);
                var bytes = AISyncExecutionCodec.EncodeBatch(new(receipt.Scope, receipt.BatchId, new[] { receipt.Operation }));
                await SendAsync(MessageType.AISyncExecutionRetry, binding, bytes, _shutdown.Token).ConfigureAwait(false);
                ++sent; ++_localRetries;
            }
    }
    private async Task ProcessAsync(Work work)
    {
        if (work.Binding != _capture())
        {
            // The transition from FrozenClient to Client can invalidate the
            // captured epoch AFTER the Host has sent its first round Offer.
            // Preserve that Offer only for this exact connected session/round.
            if (work.Kind == WorkKind.Control) DeferCurrentOffer(work);
            return;
        }
        ++_diagnosticWork;
        _maxQueueAge = Math.Max(_maxQueueAge, Environment.TickCount64 - work.Tick);
        if (Environment.TickCount64 - work.Tick > 2000)
        { await RefuseAsync(work).ConfigureAwait(false); return; }
        if (work.Kind == WorkKind.Control) await ProcessControlAsync(work).ConfigureAwait(false);
        else if (work.Kind == WorkKind.Native) await ProcessNativeAsync(work).ConfigureAwait(false);
        else if (work.Kind == WorkKind.Route) await ProcessRouteAsync(work).ConfigureAwait(false);
        else if (work.Kind == WorkKind.State)
        {
            if (!BindingCurrent(work.Binding) || _waitingReady) { await RefuseAsync(work).ConfigureAwait(false); return; }
            var value=AISyncCausalStateCodec.Decode(work.Bytes);
            AISyncExecutionCodec.RequireCurrentBinding(value.Scope, _scope!.Value);
            await SendAsync(MessageType.AISyncClientState, work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false);
            var count=++_statePackets;
            if(count<=8||(count&63)==0)_log($"Causal event forwarded to native IPC: packet={count}, serial={value.Serial}, through={value.Through}.");
        }
        else if (BindingCurrent(work.Binding) && !_waitingReady)
        {
            AISyncExecutionCodec.RequireCurrentBinding(AISyncExecutionCodec.DecodeBatchScope(work.Bytes), _scope!.Value);
            var observedBatch = AISyncExecutionCodec.DecodeBatch(work.Bytes);
            foreach (var operation in observedBatch.Records) {
                if (_diagnosticSent.ContainsKey(operation.GlobalSequence)) continue;
                if (_diagnosticSent.Count < 2048) _diagnosticSent.Add(operation.GlobalSequence, Environment.TickCount64);
                else ++_diagnosticDropped;
            }
            await SendAsync(work.Kind == WorkKind.Retry ? MessageType.AISyncExecutionRetry : MessageType.AISyncExecutionBatch,
                work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false);
        }
        else await RefuseAsync(work).ConfigureAwait(false);
    }
    private async Task ProcessRouteAsync(Work work)
    {
        if (!BindingCurrent(work.Binding) || _waitingReady)
        { await RefuseAsync(work).ConfigureAwait(false); return; }
        var key=AISyncRouteCodec.DecodeKey(work.Bytes);
        AISyncExecutionCodec.RequireCurrentBinding(key.Scope, _scope!.Value);
        await SendAsync(MessageType.AISyncClientRoute, work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false);
        var routePacket=++_routePackets;
        if(routePacket<=8 || (routePacket&63)==0)
            _log($"Host route forwarded to native IPC: packet={routePacket}, command={key.CommandId}, "
                + $"parameters={key.ParameterRevision}, route={key.RouteRevision}, part={key.Part}.");
    }
    private bool BindingCurrent(AISyncExecutionBinding binding) => binding.Active && _scope is not null
        && _nativeBinding == binding && Volatile.Read(ref _lossEpoch) != binding.Epoch;
    private async Task ProcessControlAsync(Work work)
    {
        var control = AISyncExecutionControlCodec.Decode(work.Bytes);
        if (control.Kind == AISyncExecutionControlKind.Checkpoint && _scope == control.Scope && BindingCurrent(work.Binding) && !_waitingReady) {
            await SendAsync(MessageType.AISyncExecutionBinding, work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false); return;
        }
        if (control.Kind != AISyncExecutionControlKind.Offer)
        { if (_scope == control.Scope) await RetireAsync(AISyncExecutionRecovery.RestartRequired).ConfigureAwait(false); return; }
        if (!OfferMatches(control, work.Binding))
        {
            if (DeferCurrentOffer(work)) return;
            await RefuseAsync(work).ConfigureAwait(false); return;
        }
        if (_scope == control.Scope && _nativeBinding == work.Binding)
        {
            // An exact duplicate Offer is not a new authority. Re-sending its
            // native binding is idempotent; never retire a healthy session.
            if (_waitingReady)
                await SendAsync(MessageType.AISyncExecutionBinding, work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false);
            return;
        }
        if (_scope is not null) await RetireAsync(AISyncExecutionRecovery.RestartRequired).ConfigureAwait(false);
        _scope = control.Scope; _profiles = control.Value; _nativeBinding = work.Binding; _localWaiting.Clear();
        _waitingReady = true; _openedAt = Environment.TickCount64;
        await SendAsync(MessageType.AISyncExecutionBinding, work.Binding, work.Bytes, _shutdown.Token).ConfigureAwait(false);
    }
    // The Host can offer before the new Character/world authority becomes
    // active locally. Keep only one bounded, authenticated-by-current-lobby
    // candidate; its full scope is revalidated before any native IPC send.
    private bool DeferCurrentOffer(Work work)
    {
        if (work.Kind != WorkKind.Control) return false;
        AISyncExecutionControl control;
        try { control = AISyncExecutionControlCodec.Decode(work.Bytes); }
        catch (Exception) { return false; }
        if (control.Kind != AISyncExecutionControlKind.Offer) return false;
        var current = _capture();
        if (!ReferenceEquals(current.Client, _client) || current.Client is null ||
            current.Session == Guid.Empty || current.Session != control.Scope.Session ||
            current.WorldGeneration <= 0 || (ulong)current.WorldGeneration != control.Scope.WorldEpoch ||
            current.ConnectionIdentity == Guid.Empty || current.ProcessId <= 0 ||
            current.NativeGeneration <= 0) return false;
        if (Environment.TickCount64 - work.Tick > OfferDeferMilliseconds ||
            (_scope is not null && control.Scope.AuthorityEpoch < _scope.Value.AuthorityEpoch)) return false;
        _deferredOffer = work;
        return true;
    }
    private async Task FlushDeferredOfferAsync()
    {
        var pending = _deferredOffer;
        if (pending is null) return;
        var current = _capture();
        if (Environment.TickCount64 - pending.Tick > OfferDeferMilliseconds ||
            !ReferenceEquals(current.Client, pending.Binding.Client) ||
            current.Session != pending.Binding.Session ||
            current.WorldGeneration != pending.Binding.WorldGeneration)
        {
            _deferredOffer = null; return;
        }
        if (!current.Active) return;
        _deferredOffer = null;
        var restored = pending with { Binding = current };
        var offer = AISyncExecutionControlCodec.Decode(restored.Bytes);
        if ((_scope is not null && offer.Scope.AuthorityEpoch < _scope.Value.AuthorityEpoch) ||
            !OfferMatches(offer, current)) return;
        _log($"Deferred execution Offer resumed for world={current.WorldGeneration}, native={current.NativeGeneration}.");
        await ProcessControlAsync(restored).ConfigureAwait(false);
    }
    private bool OfferMatches(AISyncExecutionControl control, AISyncExecutionBinding binding) =>
        binding.Active && ReferenceEquals(binding.Client, _client) && binding.Session == control.Scope.Session
        && binding.WorldGeneration > 0 && (ulong)binding.WorldGeneration == control.Scope.WorldEpoch
        && binding.NativeGeneration > 0 && binding.ProcessId > 0 && binding.ConnectionIdentity != Guid.Empty
        && Volatile.Read(ref _lossEpoch) != binding.Epoch;
    private async Task ProcessNativeAsync(Work work)
    {
        if (!BindingCurrent(work.Binding) || work.Native is null) return;
        if (work.Native.Type == MessageType.AISyncExecutionBinding) await ProcessNativeControlAsync(work).ConfigureAwait(false);
        else if (work.Native.Type == MessageType.AISyncExecutionReceipt && !_waitingReady)
        {
            if (AISyncExecutionCodec.DecodeReceiptScope(work.Bytes) != _scope) return;
            var receipt = AISyncExecutionCodec.DecodeReceipt(work.Bytes);
            if (receipt.Scope != _scope) return;
            if (_diagnosticSent.TryGetValue(receipt.Operation.GlobalSequence, out var started)) {
                _maxReceiptAge = Math.Max(_maxReceiptAge, Environment.TickCount64 - started);
                if (receipt.Stage != AISyncExecutionStage.Waiting && receipt.Stage != AISyncExecutionStage.Entered)
                    _diagnosticSent.Remove(receipt.Operation.GlobalSequence);
            }
            if (receipt.Stage == AISyncExecutionStage.Waiting) {
                ++_diagnosticWaiting;
                _localWaiting[receipt.Operation.GlobalSequence] = (receipt, Environment.TickCount64);
            }
            else _localWaiting.Remove(receipt.Operation.GlobalSequence);
            if (receipt.Stage == AISyncExecutionStage.Admitted) ++_diagnosticAdmitted;
            if (!work.Binding.Client!.SendAISyncExecutionReceipt(receipt))
                throw new ProtocolException("Native execution receipt lost its active consumer binding.");
        }
    }
    private async Task ProcessNativeControlAsync(Work work)
    {
        if (AISyncExecutionControlCodec.DecodeScope(work.Bytes) != _scope) return;
        var control = AISyncExecutionControlCodec.Decode(work.Bytes);
        if (_scope != control.Scope) return;
        if (control.Kind == AISyncExecutionControlKind.CheckpointAck && !_waitingReady) {
            if (!work.Binding.Client!.AcknowledgeAISyncExecutionCheckpoint(control))
                throw new ProtocolException("Native checkpoint lost its current consumer or prefix.");
            return;
        }
        if (control.Kind == AISyncExecutionControlKind.Recovery)
        {
            _log($"Native execution scope entered recovery: reason={(AISyncExecutionRecovery)control.Value}.");
            await RetireAsync((AISyncExecutionRecovery)control.Value).ConfigureAwait(false); return;
        }
        if (control.Kind != AISyncExecutionControlKind.Ready || control.Value != _profiles || !_waitingReady) return;
        if (!work.Binding.Client!.AcceptAISyncExecution(control.Scope)) throw new ProtocolException("Execution readiness rejected.");
        _waitingReady = false;
        _log($"Native execution Ready acknowledged: world={control.Scope.WorldEpoch}, authority={control.Scope.AuthorityEpoch}, consumer={control.Scope.ConsumerEpoch}.");
    }
    private async Task SendAsync(MessageType type, AISyncExecutionBinding binding, ReadOnlyMemory<byte> bytes, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(2));
        var started = Environment.TickCount64;
        await _send(type, binding.ConnectionIdentity, binding.ProcessId, bytes, timeout.Token).ConfigureAwait(false);
        _maxIpcSend = Math.Max(_maxIpcSend, Environment.TickCount64 - started);
    }
    private async Task RefuseAsync(Work work)
    {
        if (_scope is not null) await RetireAsync(AISyncExecutionRecovery.RestartRequired).ConfigureAwait(false);
        if (work.Kind != WorkKind.Control) return;
        var control = AISyncExecutionControlCodec.Decode(work.Bytes);
        if (control.Kind == AISyncExecutionControlKind.Offer)
            work.Binding.Client?.RejectAISyncExecution(control.Scope, AISyncExecutionRecovery.NativeUnavailable);
    }
    private async Task RetireAsync(AISyncExecutionRecovery reason)
    {
        var scope = _scope; var binding = _nativeBinding;
        _scope = null; _nativeBinding = null; _waitingReady = false;
        _diagnosticSent.Clear(); _localWaiting.Clear();
        // The Host reopens with a new authority epoch; an old overflow must
        // not refuse that fresh offer for the rest of the round.
        Interlocked.Exchange(ref _lossEpoch, long.MinValue);
        if (scope is null || binding is null) return;
        binding.Client?.RejectAISyncExecution(scope.Value, reason);
        var bytes = AISyncExecutionControlCodec.Encode(new(scope.Value, AISyncExecutionControlKind.Recovery, (uint)reason));
        try { await SendAsync(MessageType.AISyncExecutionBinding, binding, bytes, CancellationToken.None).ConfigureAwait(false); }
        catch (Exception exception) { _log($"Retired execution IPC binding: {exception.Message}"); }
    }
    private void LogLatency()
    {
        var now = Environment.TickCount64;
        if (_scope is null || now - _diagnosticTick < 5000) return;
        _diagnosticTick = now;
        var oldest = _diagnosticSent.Count == 0 ? 0 : now - _diagnosticSent.Values.Min();
        _log($"Execution timing: work={_diagnosticWork}, maxQueueMs={_maxQueueAge}, maxIpcSendMs={_maxIpcSend}, "
            + $"maxReceiptAgeMs={_maxReceiptAge}, waitingReceipts={_diagnosticWaiting}, admittedReceipts={_diagnosticAdmitted}, localRetries={_localRetries}, localWaiting={_localWaiting.Count}, "
            + $"pendingSamples={_diagnosticSent.Count}, oldestPendingMs={oldest}, droppedSamples={_diagnosticDropped}; "
            + "receipt age includes native/IPC/bridge waiting; cumulative timings, not network RTT.");
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        DetachClient(); _queue.Writer.TryComplete(); _shutdown.Cancel();
    }
}
