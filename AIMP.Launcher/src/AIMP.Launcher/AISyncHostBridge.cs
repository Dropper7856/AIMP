using AIMP.Protocol;
using System.Threading.Channels;

namespace AIMP.Launcher;

internal sealed record AISyncHostBinding(LobbyHost? Host, Guid Session, int WorldGeneration,
    int NativeGeneration, int ProcessId, Guid ConnectionIdentity, long Epoch, bool Active);

// The producer owns the decision stream. Fan-out changes only consumerEpoch;
// it never derives a command from a pose, shadow observation or Client packet.
internal sealed partial class AISyncHostBridge : IDisposable
{
    private sealed record Work(AISyncHostBinding Binding, NativeAISyncExecution? Native,
        ulong Peer, AISyncExecutionControl? Control, AISyncExecutionReceipt? Receipt);
    private readonly record struct CommandKey(uint A, uint B, uint Birth, uint Class,
        AISyncTransactionCapability Capability, ulong OwnerEpoch, ulong Command);
    private sealed record Waiting(AISyncTransaction Operation, long Tick);
    private static CommandKey Identity(AISyncTransaction operation) => new(operation.EntityA,
        operation.EntityB, operation.Birth, operation.CharacterClass, operation.Capability,
        operation.OwnerEpoch, operation.CommandId);
    private readonly Func<AISyncHostBinding> _capture;
    private readonly AISyncExecutionBridge.NativeSend _send;
    private readonly Action<string> _log;
    private readonly bool _enabled;
    private readonly bool _authoritativeRoutes;
    private readonly Channel<Work> _queue = Channel.CreateBounded<Work>(new BoundedChannelOptions(256)
        { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Dictionary<ulong, AISyncExecutionScope> _peers = new();
    private readonly Dictionary<(ulong Peer, ulong Sequence), Waiting> _waiting = new();
    private LobbyHost? _host;
    private AISyncHostBinding? _binding, _attempted;
    private AISyncExecutionScope? _scope;
    private AISyncExecutionJournal? _journal;
    private AISyncRouteJournal? _routes;
    private ulong _authority;
    private uint _profiles;
    private ulong _routePackets, _lastStateSerial, _statePackets;
    private bool _offered, _ready;
    private long _opened, _lossEpoch = long.MinValue, _lastReadinessLog;
    private const long SourceReadyTimeoutMilliseconds = 22000;
    // A lost scope is never resumed: a NEW authority epoch is offered after a
    // bounded backoff, once natives retired the old one (empty baseline).
    private const long MinimumReopenDelay = 2000, MaximumReopenDelay = 15000;
    private long _reopenAt, _reopenDelay = MinimumReopenDelay;
    internal int Reopens { get; private set; }
    private int _disposed;
    internal Task Completion { get; }

    internal AISyncHostBridge(Func<AISyncHostBinding> capture, AISyncExecutionBridge.NativeSend send,
        Action<string> log, bool? enabled = null, bool? authoritativeRoutes = null)
    {
        _capture = capture; _send = send; _log = log;
        _enabled = enabled ?? true;
        _authoritativeRoutes = authoritativeRoutes ?? true;
        Completion = _enabled ? Task.Run(RunAsync) : Task.CompletedTask;
    }
    internal void BindHost(LobbyHost host)
    {
        if (ReferenceEquals(_host, host)) return;
        Detach(); _host = host;
        host.AISyncExecutionControlReceived += OnControl;
        host.AISyncExecutionReceiptReceived += OnReceipt;
    }
    internal void UnbindHost(LobbyHost host) { if (ReferenceEquals(_host, host)) Detach(); }
    private void Detach()
    {
        var host = Interlocked.Exchange(ref _host, null);
        if (host is null) return;
        host.AISyncExecutionControlReceived -= OnControl;
        host.AISyncExecutionReceiptReceived -= OnReceipt;
    }
    internal void ObserveNative(NativeAISyncExecution packet)
    {
        if (packet.Type is not (MessageType.AISyncHostBatch or MessageType.AISyncHostControl or MessageType.AISyncHostRoute or MessageType.AISyncHostState)) return;
        if (packet.Bytes.Length > Math.Max(AISyncRouteCodec.MaxBytes, AISyncExecutionCodec.MaxBatchBytes))
        { MarkLoss(_capture().Epoch); return; }
        var binding = _capture();
        if (packet.ConnectionIdentity != binding.ConnectionIdentity || packet.ProcessId != binding.ProcessId) return;
        // The IPC event already owns copied memory. Preserve that incarnation.
        Enqueue(new(binding, packet with { Bytes = packet.Bytes.ToArray() }, 0, null, null));
    }
    private void OnControl(object? sender, (ulong PeerId, AISyncExecutionControl Control) value)
    {
        if (ReferenceEquals(sender, _host)) Enqueue(new(_capture(), null, value.PeerId, value.Control, null));
    }
    private void OnReceipt(object? sender, (ulong PeerId, AISyncExecutionReceipt Receipt) value)
    {
        if (ReferenceEquals(sender, _host)) Enqueue(new(_capture(), null, value.PeerId, null, value.Receipt));
    }
    private void Enqueue(Work work)
    {
        if (!_enabled || Volatile.Read(ref _disposed) != 0) return;
        if (!_queue.Writer.TryWrite(work)) MarkLoss(work.Binding.Epoch);
    }
    private void MarkLoss(long epoch)
    {
        long previous;
        do { previous = Volatile.Read(ref _lossEpoch); if (epoch <= previous) return; }
        while (Interlocked.CompareExchange(ref _lossEpoch, epoch, previous) != previous);
    }
    private async Task RunAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(20));
        try
        {
            while (await timer.WaitForNextTickAsync(_shutdown.Token).ConfigureAwait(false))
            {
                try { await TickAsync().ConfigureAwait(false); }
                catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { throw; }
                catch (Exception exception)
                { _log($"Host command stream refused: {exception.Message}"); await RetireAsync().ConfigureAwait(false); }
            }
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
        finally { await RetireAsync().ConfigureAwait(false); }
    }
    private async Task TickAsync()
    {
        await RefreshBindingAsync().ConfigureAwait(false);
        ProcessQueued();
        if (_binding is null || _scope is null) return;
        ValidateRoster();
        await OfferSourceAsync().ConfigureAwait(false);
        if (_ready) { RetryWaiting(); await CheckpointAsync().ConfigureAwait(false); }
    }
    private async Task RefreshBindingAsync()
    {
        var current = _capture();
        if (_binding is not null && (_binding != current || !current.Active ||
            Volatile.Read(ref _lossEpoch) == current.Epoch))
        {
            if (Volatile.Read(ref _lossEpoch) == current.Epoch)
                _log("Host command stream refused: bounded ingress lost coverage.");
            await RetireAsync().ConfigureAwait(false);
        }
        if (_binding is null && current.Active && ReferenceEquals(current.Host, _host) &&
            (current != _attempted || Environment.TickCount64 >= _reopenAt))
        {
            if (current == _attempted) { Reopens++; _log($"Host command stream reopening (attempt {Reopens})."); }
            await OpenAsync(current).ConfigureAwait(false);
        }
    }
    private void ProcessQueued()
    {
        for (var i = 0; i < 64 && _queue.Reader.TryRead(out var work); i++)
        {
            if (_binding != work.Binding || work.Binding != _capture()) continue;
            if (work.Native is not null) ProcessNative(work.Native);
            else ProcessPeer(work);
        }
    }
    private void ValidateRoster()
    {
        var roster = _binding!.Host!.GetAISyncExecutionRoster();
        var peers = roster.Players.Where(p => !p.IsHost).Select(p => p.PeerId).ToHashSet();
        if (roster.State != "Starting" || roster.LevelGeneration != _binding.WorldGeneration ||
            !peers.SetEquals(_peers.Keys))
            throw new ProtocolException("Host execution roster changed; a new empty baseline is required.");
        if (!_ready && Environment.TickCount64 - _opened > SourceReadyTimeoutMilliseconds)
        {
            var missing = _peers.Where(p => !_binding.Host.AISyncExecutionReady(p.Key, p.Value))
                .Select(p => p.Key.ToString()).ToArray();
            throw new ProtocolException(!_offered
                ? $"Client execution readiness missing for world {_binding.WorldGeneration}: peers={string.Join(",", missing)}."
                : $"Host native source Ready not acknowledged for world {_binding.WorldGeneration}.");
        }
    }
    private async Task OfferSourceAsync()
    {
        if (_offered) return;
        if (!_peers.All(p => _binding!.Host!.AISyncExecutionReady(p.Key, p.Value)))
        {
            var now = Environment.TickCount64;
            if (now - _lastReadinessLog >= 3000)
            {
                _lastReadinessLog = now;
                var missing = _peers.Where(p => !_binding!.Host!.AISyncExecutionReady(p.Key, p.Value))
                    .Select(p => p.Key.ToString());
                _log($"Waiting for Client native execution Ready: round={_binding!.WorldGeneration}, peers={string.Join(",", missing)}.");
            }
            return;
        }
        _offered = true;
        _log($"Client execution roster Ready, offering Host source: round={_binding!.WorldGeneration}, authority={_scope!.Value.AuthorityEpoch}.");
        await SendAsync(_binding!, new(_scope!.Value, AISyncExecutionControlKind.Offer, _profiles)).ConfigureAwait(false);
    }
    private void RetryWaiting()
    {
        foreach (var pair in RetryHeads())
        {
            if (Environment.TickCount64 - pair.Value.Tick < 20) continue;
            if (!_binding!.Host!.RetryAISyncExecution(pair.Key.Peer, pair.Key.Sequence))
            { ResolveRetryRace(pair.Key); continue; }
            _waiting[pair.Key] = pair.Value with { Tick = Environment.TickCount64 };
        }
    }
    private KeyValuePair<(ulong Peer, ulong Sequence), Waiting>[] RetryHeads() => _waiting
        .GroupBy(p => (p.Key.Peer, Actor: Identity(p.Value.Operation) with { Command = 0 }))
        .Select(group => group.MinBy(p => p.Key.Sequence)).ToArray();
    private void ResolveRetryRace((ulong Peer, ulong Sequence) key)
    {
        if (_binding!.Host!.TryGetAISyncExecutionReceipt(key.Peer, _peers[key.Peer], key.Sequence, out var receipt)
            && receipt?.Stage != AISyncExecutionStage.Waiting) { _waiting.Remove(key); return; }
        throw new ProtocolException("Waiting native operation lost its exact retry token.");
    }
    private async Task OpenAsync(AISyncHostBinding binding)
    {
        _attempted = binding;
        if (binding.Session == Guid.Empty || binding.WorldGeneration <= 0 || binding.NativeGeneration <= 0 ||
            binding.ProcessId <= 0 || binding.ConnectionIdentity == Guid.Empty || _authority == ulong.MaxValue)
            throw new ProtocolException("Incomplete Host native binding.");
        _binding = binding;
        await binding.Host!.StopAISyncExecutionAsync().ConfigureAwait(false);
        PrepareSource(binding);
        OfferConsumers(binding);
    }
    private void PrepareSource(AISyncHostBinding binding)
    {
        _scope = new(binding.Session, ++_authority, (ulong)binding.WorldGeneration, 1);
        _journal = new(_scope.Value); _lastStateSerial = 0;
        _routes = new(_scope.Value);
        _profiles = (uint)AISyncExecutionProfiles.All | (_authoritativeRoutes ? (uint)AISyncExecutionProfiles.AuthoritativeRoutes : 0);
        _opened = Environment.TickCount64; _lastReadinessLog = 0;
    }
    private void OfferConsumers(AISyncHostBinding binding)
    {
        var roster = binding.Host!.GetAISyncExecutionRoster();
        if (roster.SessionId != binding.Session || roster.State != "Starting" || roster.LevelGeneration != binding.WorldGeneration)
            throw new ProtocolException("Host command source requires a current starting round.");
        foreach (var player in roster.Players.Where(p => !p.IsHost))
            _peers.Add(player.PeerId, binding.Host.OfferAISyncExecution(player.PeerId,
                binding.WorldGeneration, _scope!.Value.AuthorityEpoch, (AISyncExecutionProfiles)_profiles));
    }
    private void ProcessNative(NativeAISyncExecution packet)
    {
        if (_scope is null || _binding is null || !_offered) return;
        if (packet.Type == MessageType.AISyncHostControl)
        { ProcessSourceControl(packet); return; }
        if (packet.Type == MessageType.AISyncHostState)
        { ProcessSourceState(packet); return; }
        if (packet.Type == MessageType.AISyncHostRoute)
        { ProcessSourceRoute(packet); return; }
        if (AISyncExecutionCodec.DecodeBatchScope(packet.Bytes.Span) != _scope) return;
        if (!_ready) throw new ProtocolException("Host command preceded native readiness.");
        var batch = AISyncExecutionCodec.DecodeBatch(packet.Bytes.Span, _scope.Value);
        if (!RetainSourceBatch(packet)) return;
        if (!_routes!.Observe(batch)) throw new ProtocolException("Host route command correlation lost coverage.");
        PublishBatch(batch);
    }
    private void ProcessSourceRoute(NativeAISyncExecution packet)
    {
        var key = AISyncRouteCodec.DecodeKey(packet.Bytes.Span);
        if (key.Scope != _scope) return;
        if (!_ready) throw new ProtocolException("Host route preceded native readiness.");
        var result = _routes!.Append(packet.Bytes.Span);
        var routePacket=++_routePackets;
        if(routePacket<=8 || (routePacket&63)==0)
            _log($"Host route packet {routePacket}: result={result}, command={key.CommandId}, "
                + $"parameters={key.ParameterRevision}, route={key.RouteRevision}, part={key.Part}, peers={_peers.Count}.");
        if (result is AISyncExecutionJournalResult.Duplicate or AISyncExecutionJournalResult.Rejected) return;
        if (result != AISyncExecutionJournalResult.Accepted)
            throw new ProtocolException("Host route has no live exact AIEX command or conflicts with retained bytes.");
        var graph = AISyncRouteCodec.Decode(_routes.Retained(key), key);
        foreach (var peer in _peers)
        {
            // The source command was published to this exact consumer already.
            var scoped = graph with { Key = key with { Scope = key.Scope with { ConsumerEpoch = peer.Value.ConsumerEpoch } } };
            if (_binding!.Host!.PublishAISyncRoute(peer.Key, AISyncRouteCodec.Encode(scoped)) != AISyncExecutionJournalResult.Accepted)
                throw new ProtocolException("A consumer failed to retain its correlated solved route.");
        }
    }
    private void ProcessSourceControl(NativeAISyncExecution packet)
    {
        if (AISyncExecutionControlCodec.DecodeScope(packet.Bytes.Span) != _scope) return;
        var control = AISyncExecutionControlCodec.Decode(packet.Bytes.Span);
        if (control.Scope != _scope) return;
        if (control.Kind == AISyncExecutionControlKind.CheckpointAck) { CompleteCheckpoint(control); return; }
        if (control.Kind != AISyncExecutionControlKind.Ready || control.Value != _profiles)
            throw new ProtocolException($"Host native source lost command coverage ({control.Kind}, reason={control.Value}).");
        _ready = true; _reopenDelay = MinimumReopenDelay;
    }
    private bool RetainSourceBatch(NativeAISyncExecution packet)
    {
        var result = _journal!.Append(packet.Bytes.Span);
        if (result is AISyncExecutionJournalResult.Duplicate or AISyncExecutionJournalResult.Rejected) return false;
        if (result != AISyncExecutionJournalResult.Accepted)
            throw new ProtocolException("Host command journal lost ordered coverage.");
        return true;
    }
    private void PublishBatch(AISyncExecutionBatch batch)
    {
        foreach (var peer in _peers)
        {
            var records = batch.Records.Select(r => r with { ConsumerEpoch = peer.Value.ConsumerEpoch }).ToArray();
            var bytes = AISyncExecutionCodec.EncodeBatch(new(peer.Value, batch.BatchId, records));
            if (_binding!.Host!.PublishAISyncExecution(peer.Key, bytes) != AISyncExecutionJournalResult.Accepted)
                throw new ProtocolException("A consumer failed to retain the complete command batch.");
        }
        // All peer senders retained the batch; this is not a native execution ACK.
    }
    private void ProcessPeer(Work work)
    {
        if (!_peers.TryGetValue(work.Peer, out var scope)) return;
        if (AcceptCheckpointAck(work, scope)) return;
        if (work.Control is { } control && control.Scope == scope && control.Kind != AISyncExecutionControlKind.Ready)
            throw new ProtocolException($"Client command scope entered recovery: {work.Control?.Kind}, reason={work.Control?.Value}.");
        if (work.Receipt is not { } receipt || receipt.Scope != scope) return;
        // LobbyHost raises receipts only after its per-peer journal validates
        // the exact batch/operation echo and legal disposition transition.
        // A refusal settles retry eligibility, not native ownership or cleanup.
        var key = (work.Peer, receipt.Operation.GlobalSequence);
        if (receipt.Stage is AISyncExecutionStage.Rejected or AISyncExecutionStage.Unknown)
        {
            _waiting.Remove(key);
            _log($"Client native command refused: peer={work.Peer}, stage={receipt.Stage}, result={receipt.Result}, "
                + $"sequence={receipt.Operation.GlobalSequence}, command={receipt.Operation.CommandId}, "
                + $"kind={receipt.Operation.NativeKind}, opcode={receipt.Operation.Opcode}.");
            // Leave the final receipt in the consumer journal. In particular,
            // Unknown cannot authorize a retry, replacement, or checkpoint of
            // resources whose release has not been proved by native execution.
            return;
        }
        if (receipt.Stage == AISyncExecutionStage.Waiting)
            _waiting[key] = new(receipt.Operation, Environment.TickCount64);
        else if (receipt.Stage is AISyncExecutionStage.Entered or AISyncExecutionStage.Admitted
            or AISyncExecutionStage.Released or AISyncExecutionStage.Superseded)
            _waiting.Remove(key);
    }
    private async Task SendAsync(AISyncHostBinding binding, AISyncExecutionControl control)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        await _send(MessageType.AISyncHostBinding, binding.ConnectionIdentity, binding.ProcessId,
            AISyncExecutionControlCodec.Encode(control), timeout.Token).ConfigureAwait(false);
    }
    private async Task RetireAsync()
    {
        var binding = _binding; var scope = _scope;
        _binding = null; _scope = null; _journal = null; _ready = false; _offered = false;
        _routes?.Fence(); _routes = null;
        _peers.Clear(); _waiting.Clear(); ResetCheckpoint();
        Interlocked.Exchange(ref _lossEpoch, long.MinValue);
        if (binding is null) return;
        _reopenAt = Environment.TickCount64 + _reopenDelay;
        _reopenDelay = Math.Min(_reopenDelay * 2, MaximumReopenDelay);
        if (scope is not null)
        {
            try { await SendAsync(binding, new(scope.Value, AISyncExecutionControlKind.Close, 0)).ConfigureAwait(false); }
            catch (Exception exception) { _log($"Host source retirement IPC: {exception.Message}"); }
        }
        try { await binding.Host!.StopAISyncExecutionAsync().ConfigureAwait(false); }
        catch (Exception exception) { _log($"Host consumer retirement: {exception.Message}"); }
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        Detach(); _queue.Writer.TryComplete(); _shutdown.Cancel();
    }
}
