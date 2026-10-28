namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private sealed class ExecutionPeer(ClientState state, AISyncExecutionControl offer, AISyncExecutionSender sender)
    {
        internal ClientState State { get; } = state;
        internal AISyncExecutionControl Offer { get; } = offer;
        internal AISyncExecutionSender Sender { get; } = sender;
        internal AISyncExecutionJournal Journal { get; } = new(offer.Scope);
        internal ulong LastCarrierSerial;
        internal AISyncRouteJournal Routes { get; } = new(offer.Scope);
        internal bool Ready, Fenced;
        internal ulong CheckpointPending;
    }
    private readonly Dictionary<ulong, ExecutionPeer> _executionPeers = new();
    private readonly Dictionary<ulong, Task> _executionClosing = new();
    private Task _executionStopTask = Task.CompletedTask;
    private ulong _executionConsumer, _executionAuthority;
    public event EventHandler<(ulong PeerId, AISyncExecutionControl Control)>? AISyncExecutionControlReceived;
    public event EventHandler<(ulong PeerId, AISyncExecutionReceipt Receipt)>? AISyncExecutionReceiptReceived;

    // Snapshot only: obtaining the roster does not grant a peer a publisher role.
    public LobbySnapshotPayload GetAISyncExecutionRoster() => CreateSnapshot();

    // Explicit API only. A peer cannot negotiate itself into the Host's publisher role.
    public AISyncExecutionScope OfferAISyncExecution(ulong peerId, int worldGeneration, ulong authorityEpoch,
        AISyncExecutionProfiles profiles = AISyncExecutionProfiles.All)
    {
        lock (_stateLock)
        {
            ObjectDisposedException.ThrowIf(_disposed != 0, this);
            if (!_executionStopTask.IsCompleted || _executionClosing.ContainsKey(peerId) ||
                _state != "Starting" || worldGeneration != _levelGeneration || authorityEpoch == 0
                || authorityEpoch < _executionAuthority || !_clients.TryGetValue(peerId, out var state)
                || (_executionPeers.Count != 0 && authorityEpoch != _executionAuthority)
                || _executionPeers.ContainsKey(peerId) || _executionConsumer == ulong.MaxValue)
                throw new InvalidOperationException("AISync execution requires a fresh, active peer scope.");
            var scope = new AISyncExecutionScope(SessionId, authorityEpoch, (ulong)worldGeneration, _executionConsumer + 1);
            var offer = new AISyncExecutionControl(scope, AISyncExecutionControlKind.Offer, (uint)profiles);
            var packet = AISyncExecutionControlCodec.Encode(offer);
            var sender = new AISyncExecutionSender(state.Connection.SendExecutionAsync,
                () => ExecutionSendFailed(peerId, scope), _shutdown.Token);
            var peer = new ExecutionPeer(state, offer, sender);
            _executionPeers.Add(peerId, peer); ++_executionConsumer; _executionAuthority = authorityEpoch;
            if (!sender.Enqueue(MessageType.AISyncExecutionControl, packet)) FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.Capacity);
            return scope;
        }
    }
    public bool AISyncExecutionReady(ulong peerId, AISyncExecutionScope scope)
    {
        lock (_stateLock) return _state == "Starting" && (ulong)_levelGeneration == scope.WorldEpoch
            && _executionPeers.TryGetValue(peerId, out var peer) && peer.Offer.Scope == scope && peer.Ready && !peer.Fenced;
    }
    public AISyncExecutionJournalResult PublishAISyncExecution(ulong peerId, ReadOnlySpan<byte> bytes)
    {
        lock (_stateLock)
        {
            if (_disposed != 0 || _state != "Starting" || !_executionPeers.TryGetValue(peerId, out var peer)
                || !peer.Ready || peer.Fenced || peer.Offer.Scope.WorldEpoch != (ulong)_levelGeneration)
                return AISyncExecutionJournalResult.Rejected;
            var batch = AISyncExecutionCodec.DecodeBatch(bytes, peer.Offer.Scope);
            if (!ExecutionProfilesMatch(batch, peer.Offer.Value)) return AISyncExecutionJournalResult.Rejected;
            var result = peer.Journal.Append(bytes);
            if (result == AISyncExecutionJournalResult.RecoveryRequired
                || (result == AISyncExecutionJournalResult.Accepted && !peer.Routes.Observe(batch))
                || !peer.Sender.Enqueue(MessageType.AISyncExecutionNetworkBatch, bytes))
            {
                FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.Capacity);
                return AISyncExecutionJournalResult.RecoveryRequired;
            }
            return result;
        }
    }
    private static bool ExecutionProfilesMatch(AISyncExecutionBatch batch, uint profiles) =>
        batch.Records.All(record => AISyncExecutionControlCodec.Allows(profiles, record));
    public bool RetryAISyncExecution(ulong peerId, ulong sequence)
    {
        lock (_stateLock)
        {
            if (_state != "Starting" || !_executionPeers.TryGetValue(peerId, out var peer) || !peer.Ready || peer.Fenced
                || peer.Offer.Scope.WorldEpoch != (ulong)_levelGeneration
                || !peer.Journal.TryGetReceipt(sequence, out var receipt) || receipt?.Stage != AISyncExecutionStage.Waiting) return false;
            var bytes = AISyncExecutionCodec.EncodeBatch(new(receipt.Scope, receipt.BatchId, new[] { receipt.Operation }));
            if (peer.Sender.Enqueue(MessageType.AISyncExecutionNetworkRetry, bytes)) return true;
            FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.Capacity); return false;
        }
    }
    public bool TryGetAISyncExecutionReceipt(ulong peerId, AISyncExecutionScope scope, ulong sequence,
        out AISyncExecutionReceipt? receipt)
    {
        receipt=null;
        lock (_stateLock) return _state=="Starting" && _executionPeers.TryGetValue(peerId,out var peer)
            && peer.Ready && !peer.Fenced && peer.Offer.Scope==scope && peer.Journal.TryGetReceipt(sequence,out receipt);
    }

    private void HandleAISyncExecutionControl(ClientState state, ReadOnlyMemory<byte> bytes)
    {
        var control = AISyncExecutionControlCodec.Decode(bytes.Span);
        if (control.Scope.Session != SessionId || control.Kind is not (AISyncExecutionControlKind.Ready or AISyncExecutionControlKind.Recovery or AISyncExecutionControlKind.CheckpointAck))
            throw new ProtocolException("Invalid Client AISync execution direction or session.");
        lock (_stateLock)
        {
            if (_state != "Starting" || control.Scope.WorldEpoch != (ulong)_levelGeneration
                || !_executionPeers.TryGetValue(state.PeerId, out var peer) || !ReferenceEquals(state, peer.State)
                || peer.Offer.Scope != control.Scope || peer.Fenced) return;
            if (control.Kind == AISyncExecutionControlKind.Ready)
            {
                if (control.Value != peer.Offer.Value) throw new ProtocolException("AISync execution profile mismatch.");
                peer.Ready = true;
            }
            else if(control.Kind==AISyncExecutionControlKind.CheckpointAck) AcknowledgeExecutionCheckpoint(peer,control);
            else FenceExecutionPeerLocked(peer, (AISyncExecutionRecovery)control.Value);
        }
        RaiseExecutionControl(state.PeerId, control);
    }
    private void HandleAISyncExecutionReceipt(ClientState state, ReadOnlyMemory<byte> bytes)
    {
        var receipt = AISyncExecutionCodec.DecodeReceipt(bytes.Span);
        if (receipt.Scope.Session != SessionId) throw new ProtocolException("Invalid AISync receipt session.");
        lock (_stateLock)
        {
            if (_state != "Starting" || !_executionPeers.TryGetValue(state.PeerId, out var peer)
                || !ReferenceEquals(state, peer.State) || !peer.Ready || peer.Fenced
                || peer.Offer.Scope.WorldEpoch != (ulong)_levelGeneration) return;
            if (receipt.Scope != peer.Offer.Scope) return;
            if (receipt.Operation.GlobalSequence <= peer.Journal.HistoryFloor) return;
            if (peer.Journal.Receive(receipt) == AISyncExecutionJournalResult.RecoveryRequired)
            { FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.InvalidStream); return; }
        }
        try { AISyncExecutionReceiptReceived?.Invoke(this, (state.PeerId, receipt)); } catch (Exception) { }
    }
    private void FenceExecutionPeerLocked(ExecutionPeer peer, AISyncExecutionRecovery reason)
    {
        if (peer.Fenced) return;
        peer.Fenced = true; peer.Ready = false; peer.Journal.Fence(); peer.Routes.Fence();
        peer.Sender.Finish(new(peer.Offer.Scope, AISyncExecutionControlKind.Recovery, (uint)reason));
    }
    private void ExecutionSendFailed(ulong peerId, AISyncExecutionScope scope)
    {
        ProtocolConnection connection;
        lock (_stateLock)
        {
            if (!_executionPeers.TryGetValue(peerId, out var peer) || peer.Offer.Scope != scope) return;
            peer.Fenced = true; peer.Ready = false; peer.Journal.Fence(); peer.Routes.Fence(); connection = peer.State.Connection;
        }
        connection.Client.Dispose();
        RaiseExecutionControl(peerId, new(scope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.TransportLost));
    }
    private void RaiseExecutionControl(ulong peerId, AISyncExecutionControl control)
    { try { AISyncExecutionControlReceived?.Invoke(this, (peerId, control)); } catch (Exception) { } }

    private async Task RemoveExecutionPeerAsync(ulong peerId)
    {
        ExecutionPeer? peer;
        lock (_stateLock) { _executionPeers.Remove(peerId, out peer); if (peer is not null) { peer.Fenced = true; peer.Routes.Fence(); } }
        if (peer is not null) await peer.Sender.DisposeAsync().ConfigureAwait(false);
    }
    public Task CloseAISyncExecutionPeerAsync(ulong peerId)
    {
        lock (_stateLock)
        {
            if (_executionClosing.TryGetValue(peerId, out var closing)) return closing;
            if (!_executionPeers.Remove(peerId, out var peer)) return Task.CompletedTask;
            peer.Fenced = true; peer.Ready = false; peer.Journal.Fence(); peer.Routes.Fence();
            peer.Sender.Finish(new(peer.Offer.Scope, AISyncExecutionControlKind.Close, 0));
            var completion = CloseExecutionPeerAsync(peerId, peer);
            if (!completion.IsCompleted) _executionClosing.Add(peerId, completion);
            return completion;
        }
    }
    private async Task CloseExecutionPeerAsync(ulong peerId, ExecutionPeer peer)
    {
        try { await DrainExecutionPeerAsync(peer).ConfigureAwait(false); }
        finally { lock (_stateLock) _executionClosing.Remove(peerId); }
    }
    private static async Task DrainExecutionPeerAsync(ExecutionPeer peer)
    {
        await peer.Sender.Completion.ConfigureAwait(false);
        await peer.Sender.DisposeAsync().ConfigureAwait(false);
    }
    public Task StopAISyncExecutionAsync()
    {
        lock (_stateLock)
        {
            if (!_executionStopTask.IsCompleted) return _executionStopTask;
            var peers = _executionPeers.Values.ToArray(); _executionPeers.Clear();
            foreach (var peer in peers)
            {
                peer.Fenced = true; peer.Ready = false; peer.Journal.Fence(); peer.Routes.Fence();
                peer.Sender.Finish(new(peer.Offer.Scope, AISyncExecutionControlKind.Close, 0));
            }
            var closing = _executionClosing.Values.ToArray();
            _executionStopTask = Task.WhenAll(peers.Select(DrainExecutionPeerAsync).Concat(closing));
            return _executionStopTask;
        }
    }
}
