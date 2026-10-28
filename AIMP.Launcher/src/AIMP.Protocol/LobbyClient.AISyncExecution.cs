namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private readonly object _executionGate = new();
    private AISyncExecutionControl? _executionOffer;
    private AISyncExecutionJournal? _executionJournal;
    private AISyncRouteJournal? _executionRoutes;
    private AISyncExecutionSender? _executionSender;
    private bool _executionReady, _executionFenced;
    private ulong _executionLastConsumer, _executionLastAuthority;
    public event EventHandler<AISyncExecutionControl>? AISyncExecutionControlReceived;
    public event EventHandler<AISyncExecutionBatch>? AISyncExecutionBatchReceived;
    public event EventHandler<AISyncExecutionBatch>? AISyncExecutionRetryReceived;

    // Called only AFTER the current native IPC instance acknowledged this binding.
    // Ready negotiates the transport; it confers no local actor ownership.
    public bool AcceptAISyncExecution(AISyncExecutionScope currentNativeScope)
    {
        lock (_executionGate)
        {
            if (_executionOffer is null || _executionOffer.Scope != currentNativeScope || _executionFenced
                || !ExecutionGenerationActive(currentNativeScope) || _executionSender is null) return false;
            _executionReady = true;
            var ready = _executionOffer with { Kind = AISyncExecutionControlKind.Ready };
            if (_executionSender.Enqueue(MessageType.AISyncExecutionControl, AISyncExecutionControlCodec.Encode(ready))) return true;
            FenceClientExecutionLocked(AISyncExecutionRecovery.Capacity);
        }
        RaiseClientExecutionControl(new(currentNativeScope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.Capacity));
        return false;
    }
    public bool SendAISyncExecutionReceipt(AISyncExecutionReceipt receipt)
    {
        lock (_executionGate)
        {
            if (!_executionReady || _executionFenced || _executionJournal is null || _executionSender is null
                || !ExecutionGenerationActive(receipt.Scope) || _executionOffer?.Scope != receipt.Scope) return false;
            if (_executionJournal.Receive(receipt) == AISyncExecutionJournalResult.RecoveryRequired
                || !_executionSender.Enqueue(MessageType.AISyncExecutionNetworkReceipt, AISyncExecutionCodec.EncodeReceipt(receipt)))
                FenceClientExecutionLocked(AISyncExecutionRecovery.InvalidStream);
            else return true;
        }
        RaiseClientExecutionControl(new(receipt.Scope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.InvalidStream));
        return false;
    }
    public void RejectAISyncExecution(AISyncExecutionScope scope, AISyncExecutionRecovery reason)
    {
        var recovery = new AISyncExecutionControl(scope, AISyncExecutionControlKind.Recovery, (uint)reason);
        _ = AISyncExecutionControlCodec.Encode(recovery);
        lock (_executionGate)
        {
            if (_executionOffer?.Scope != scope || _executionFenced) return;
            FenceClientExecutionLocked(reason);
        }
        RaiseClientExecutionControl(recovery);
    }
    private bool ExecutionGenerationActive(AISyncExecutionScope scope) => scope.Session == SessionId
        && Snapshot?.State == "Starting" && (ulong)Snapshot.LevelGeneration == scope.WorldEpoch;

    private async Task HandleAISyncExecutionControlAsync(ReadOnlyMemory<byte> bytes)
    {
        var control = AISyncExecutionControlCodec.Decode(bytes.Span);
        if (control.Scope.Session != SessionId || control.Kind is AISyncExecutionControlKind.Ready or AISyncExecutionControlKind.CheckpointAck)
            throw new ProtocolException("Invalid Host AISync execution direction or session.");
        AISyncExecutionSender? previous = null;
        lock (_executionGate)
        {
            if (control.Kind == AISyncExecutionControlKind.Offer)
            {
                if (_executionOffer == control) return;
                if (!ExecutionGenerationActive(control.Scope) || control.Scope.ConsumerEpoch <= _executionLastConsumer
                    || control.Scope.AuthorityEpoch < _executionLastAuthority) return;
                previous = _executionSender;
                _executionOffer = control; _executionJournal = new(control.Scope); _executionRoutes = new(control.Scope);
                _executionReady = false; _executionFenced = false;
                _executionCheckpoint = 0; _executionCarrierSerial = 0;
                _executionLastConsumer = control.Scope.ConsumerEpoch; _executionLastAuthority = control.Scope.AuthorityEpoch;
                _executionSender = new AISyncExecutionSender(_connection.SendExecutionAsync,
                    () => ClientExecutionSendFailed(control.Scope), _shutdown.Token);
            }
            else
            {
                if (_executionOffer?.Scope != control.Scope) return;
                if (control.Kind == AISyncExecutionControlKind.Checkpoint) ReceiveExecutionCheckpoint(control);
                else {
                    _executionFenced = true; _executionReady = false; _executionJournal?.Fence(); _executionRoutes?.Fence();
                    previous = _executionSender; _executionSender = null;
                }
            }
        }
        if (previous is not null) await previous.DisposeAsync().ConfigureAwait(false);
        RaiseClientExecutionControl(control);
    }
    private void HandleAISyncExecutionBatch(ReadOnlyMemory<byte> bytes, bool retry = false)
    {
        AISyncExecutionBatch batch;
        var invalid = false;
        lock (_executionGate)
        {
            if (!_executionReady || _executionFenced || _executionOffer is null || _executionJournal is null
                || !ExecutionGenerationActive(_executionOffer.Scope)) return;
            var scope = AISyncExecutionCodec.DecodeBatchScope(bytes.Span);
            if (scope != _executionOffer.Scope) return; // Never let a stale body poison a new binding.
            batch = AISyncExecutionCodec.DecodeBatch(bytes.Span, scope);
            if (batch.Records.All(r => r.GlobalSequence <= _executionJournal.HistoryFloor)) return;
            var retained = retry ? AISyncExecutionJournalResult.Duplicate : _executionJournal.Append(bytes.Span);
            if (!batch.Records.All(r => AISyncExecutionControlCodec.Allows(_executionOffer.Value, r))
                || (retry ? batch.Records.Length != 1 || !_executionJournal.ContainsOperation(batch.BatchId, batch.Records[0])
                    : retained == AISyncExecutionJournalResult.RecoveryRequired)
                || (retained == AISyncExecutionJournalResult.Accepted && _executionRoutes?.Observe(batch) != true))
            { FenceClientExecutionLocked(AISyncExecutionRecovery.InvalidStream); invalid = true; }
        }
        if (invalid) {
            RaiseClientExecutionControl(new(batch.Scope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.InvalidStream));
            return;
        }
        // Exact duplicates may be forwarded again: only the native ledger can
        // report their disposition. Receipt/Entered/Admitted are never invented here.
        try {
            if (retry) AISyncExecutionRetryReceived?.Invoke(this, batch);
            else AISyncExecutionBatchReceived?.Invoke(this, batch);
        }
        catch (Exception) { RejectAISyncExecution(batch.Scope, AISyncExecutionRecovery.NativeUnavailable); }
    }
    private void FenceClientExecutionLocked(AISyncExecutionRecovery reason)
    {
        if (_executionFenced || _executionOffer is null) return;
        _executionFenced = true; _executionReady = false; _executionJournal?.Fence(); _executionRoutes?.Fence();
        _executionSender?.Finish(new(_executionOffer.Scope, AISyncExecutionControlKind.Recovery, (uint)reason));
    }
    private void ClientExecutionSendFailed(AISyncExecutionScope scope)
    {
        lock (_executionGate)
        {
            if (_executionOffer?.Scope != scope) return;
            _executionFenced = true; _executionReady = false; _executionJournal?.Fence(); _executionRoutes?.Fence();
        }
        _connection.Client.Dispose();
        RaiseClientExecutionControl(new(scope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.TransportLost));
    }
    private void RaiseClientExecutionControl(AISyncExecutionControl control)
    { try { AISyncExecutionControlReceived?.Invoke(this, control); } catch (Exception) { } }
    private async Task ClearAISyncExecutionAsync()
    {
        AISyncExecutionSender? sender; AISyncExecutionControl? offer;
        lock (_executionGate)
        {
            offer = _executionOffer; _executionOffer = null; _executionReady = false; _executionFenced = true;
            _executionJournal?.Fence(); _executionJournal = null; _executionRoutes?.Fence(); _executionRoutes = null;
            sender = _executionSender; _executionSender = null;
        }
        if (offer is not null) RaiseClientExecutionControl(new(offer.Scope, AISyncExecutionControlKind.Recovery,
            (uint)AISyncExecutionRecovery.TransportLost));
        if (sender is not null) await sender.DisposeAsync().ConfigureAwait(false);
    }
}
