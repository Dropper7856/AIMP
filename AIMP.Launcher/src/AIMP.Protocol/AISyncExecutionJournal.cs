namespace AIMP.Protocol;

public enum AISyncExecutionJournalResult { Accepted, Duplicate, Rejected, RecoveryRequired }

// One bounded causal stream per consumer. No eviction/compaction without a
// semantic checkpoint; receipt retention is not evidence of native execution.
// Callers serialize access. Every byte array crossing this API is copied.
public sealed class AISyncExecutionJournal
{
    private sealed record Entry(ulong BatchId, byte[] Operation);
    private readonly Dictionary<ulong, byte[]> _batches = new();
    private readonly Dictionary<ulong, Entry> _operations = new();
    private readonly Dictionary<ulong, AISyncExecutionReceipt> _receipts = new();
    private readonly Dictionary<(uint, uint, uint, uint, ulong), ulong> _owners = new();
    private readonly int _maxRecords, _maxBytes;
    private ulong _lastBatch, _lastSequence;
    public ulong HistoryFloor { get; private set; }
    public ulong LastSequence => _lastSequence;
    public AISyncExecutionScope Scope { get; }
    public int RecordCount => _operations.Count;
    public int RetainedBytes { get; private set; }
    public bool RecoveryRequired { get; private set; }
    public AISyncExecutionJournal(AISyncExecutionScope scope, int maxRecords = 2048, int maxBytes = 2 * 1024 * 1024)
    {
        AISyncExecutionCodec.RequireCurrentBinding(scope, scope);
        if (maxRecords is < 1 or > 2048 || maxBytes is < 1 or > 2 * 1024 * 1024)
            throw new ArgumentOutOfRangeException(nameof(maxRecords));
        Scope = scope; _maxRecords = maxRecords; _maxBytes = maxBytes;
    }
    public AISyncExecutionJournalResult Append(ReadOnlySpan<byte> bytes)
    {
        if (RecoveryRequired) return AISyncExecutionJournalResult.RecoveryRequired;
        var batch = AISyncExecutionCodec.DecodeBatch(bytes, Scope);
        if (batch.Records[^1].GlobalSequence <= HistoryFloor) return AISyncExecutionJournalResult.Rejected;
        if (_batches.TryGetValue(batch.BatchId, out var previous))
            return previous.AsSpan().SequenceEqual(bytes) ? AISyncExecutionJournalResult.Duplicate : Fail();
        if (_lastBatch == ulong.MaxValue || batch.BatchId != _lastBatch + 1) return Fail();
        var operations = batch.Records.Select(r => AISyncExecutionCodec.EncodeOperation(r)).ToArray();
        var retainedCost = bytes.Length + operations.Sum(b => b.Length);
        if (RecordCount + batch.Records.Length > _maxRecords || retainedCost > _maxBytes - RetainedBytes)
            return Fail();
        var owners = new Dictionary<(uint, uint, uint, uint, ulong), ulong>(_owners);
        var sequence = _lastSequence;
        foreach (var record in batch.Records)
        {
            if (sequence == ulong.MaxValue || record.GlobalSequence != sequence + 1) return Fail();
            var key = (record.EntityA, record.EntityB, record.Birth, record.CharacterClass, record.OwnerEpoch);
            owners.TryGetValue(key, out var ownerSequence);
            if (ownerSequence == ulong.MaxValue || record.OwnerSequence != ownerSequence + 1) return Fail();
            owners[key] = record.OwnerSequence; sequence = record.GlobalSequence;
        }
        if (owners.Count > 128) return Fail();
        _owners.Clear(); foreach (var owner in owners) _owners.Add(owner.Key, owner.Value);
        for (var i = 0; i < batch.Records.Length; i++)
            _operations.Add(batch.Records[i].GlobalSequence, new(batch.BatchId, operations[i]));
        _batches.Add(batch.BatchId, bytes.ToArray()); RetainedBytes += retainedCost;
        _lastBatch = batch.BatchId; _lastSequence = sequence;
        return AISyncExecutionJournalResult.Accepted;
    }
    public bool TryReadBatch(ulong batchId, out byte[] bytes)
    {
        bytes = Array.Empty<byte>();
        if (!_batches.TryGetValue(batchId, out var retained)) return false;
        bytes = retained.ToArray(); return true;
    }
    public bool ContainsOperation(ulong batchId, AISyncTransaction operation) => !RecoveryRequired
        && _operations.TryGetValue(operation.GlobalSequence, out var entry) && entry.BatchId == batchId
        && entry.Operation.AsSpan().SequenceEqual(AISyncExecutionCodec.EncodeOperation(operation));
    public AISyncExecutionJournalResult Receive(AISyncExecutionReceipt receipt)
    {
        if (RecoveryRequired) return AISyncExecutionJournalResult.RecoveryRequired;
        AISyncExecutionCodec.RequireCurrentBinding(receipt.Scope, Scope);
        _ = AISyncExecutionCodec.EncodeReceipt(receipt);
        if (!_operations.TryGetValue(receipt.Operation.GlobalSequence, out var entry)
            || entry.BatchId != receipt.BatchId
            || !entry.Operation.AsSpan().SequenceEqual(AISyncExecutionCodec.EncodeOperation(receipt.Operation))) return Fail();
        if (!ValidDisposition(receipt)) return Fail();
        if (_receipts.TryGetValue(receipt.Operation.GlobalSequence, out var previous))
        {
            if (previous == receipt) return AISyncExecutionJournalResult.Duplicate;
            if (previous.Stage is not (AISyncExecutionStage.Received or AISyncExecutionStage.Waiting or AISyncExecutionStage.Entered))
                return Fail();
            if (Rank(receipt.Stage) < Rank(previous.Stage)) return Fail();
        }
        var encoded = AISyncExecutionCodec.EncodeReceipt(receipt);
        var cost = _receipts.ContainsKey(receipt.Operation.GlobalSequence) ? 0 : encoded.Length;
        if (cost > _maxBytes - RetainedBytes) return Fail();
        // Decode a private copy: callers can neither mutate the retained operation nor a receipt.
        _receipts[receipt.Operation.GlobalSequence] = AISyncExecutionCodec.DecodeReceipt(encoded, Scope);
        RetainedBytes += cost;
        return AISyncExecutionJournalResult.Accepted;
    }
    public bool TryGetReceipt(ulong sequence, out AISyncExecutionReceipt? receipt)
    {
        receipt = null;
        if (!_receipts.TryGetValue(sequence, out var retained)) return false;
        receipt = AISyncExecutionCodec.DecodeReceipt(AISyncExecutionCodec.EncodeReceipt(retained), Scope); return true;
    }
    public void Fence() => RecoveryRequired = true;
    // Only complete batches whose native dispositions are final are candidates.
    // Received acknowledges metadata, never execution of Submit/Update/cleanup.
    public ulong SettledThrough()
    {
        var through = HistoryFloor;
        foreach (var packet in _batches.OrderBy(p => p.Key))
        {
            var batch = AISyncExecutionCodec.DecodeBatch(packet.Value, Scope);
            if (batch.Records.Any(r => !IsSettled(r.GlobalSequence))) break;
            through = batch.Records[^1].GlobalSequence;
        }
        return through;
    }
    private bool IsSettled(ulong sequence) => _receipts.TryGetValue(sequence, out var r)
        && (r.Stage is AISyncExecutionStage.Admitted or AISyncExecutionStage.Released or AISyncExecutionStage.Superseded
            || r.Stage == AISyncExecutionStage.Received && r.Operation.Opcode is
                AISyncTransactionOpcode.Begin or AISyncTransactionOpcode.Complete);
    // externalSettlement is reserved for the Host source journal after every
    // consumer's native checkpoint ACK. It is never based on network delivery.
    public bool Checkpoint(ulong through, bool externalSettlement = false)
    {
        if (RecoveryRequired || through == 0 || through < HistoryFloor || through > _lastSequence) return false;
        if (through == HistoryFloor) return true;
        var prefix = _operations.Keys.Where(s => s <= through).ToArray();
        if (!externalSettlement && prefix.Any(s => !IsSettled(s))) return false;
        var packets = _batches.Where(p => AISyncExecutionCodec.DecodeBatch(p.Value, Scope).Records[0].GlobalSequence <= through).ToArray();
        if (packets.Any(p => AISyncExecutionCodec.DecodeBatch(p.Value, Scope).Records[^1].GlobalSequence > through)) return false;
        foreach (var sequence in prefix)
        {
            RetainedBytes -= _operations[sequence].Operation.Length; _operations.Remove(sequence);
            if (_receipts.Remove(sequence, out var receipt)) RetainedBytes -= AISyncExecutionCodec.EncodeReceipt(receipt).Length;
        }
        foreach (var packet in packets) { RetainedBytes -= packet.Value.Length; _batches.Remove(packet.Key); }
        HistoryFloor = through; return true;
    }
    private AISyncExecutionJournalResult Fail() { Fence(); return AISyncExecutionJournalResult.RecoveryRequired; }
    private static int Rank(AISyncExecutionStage stage) => stage switch
    {
        AISyncExecutionStage.Received => 1, AISyncExecutionStage.Waiting => 2, AISyncExecutionStage.Entered => 3, _ => 4
    };
    private static bool ValidDisposition(AISyncExecutionReceipt r)
    {
        var metadata = r.Operation.Opcode is AISyncTransactionOpcode.Begin or AISyncTransactionOpcode.Complete;
        return r.Stage switch
        {
            AISyncExecutionStage.Received => r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Waiting => !metadata && r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Entered => !metadata && r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Admitted => !metadata && r.Operation.Opcode != AISyncTransactionOpcode.Release && r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Released => r.Operation.Opcode == AISyncTransactionOpcode.Release && r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Unknown => !metadata && r.Result == AISyncExecutionResult.OutcomeUnknown,
            AISyncExecutionStage.Superseded => r.Operation.Opcode is AISyncTransactionOpcode.Submit or AISyncTransactionOpcode.Update
                && r.Result == AISyncExecutionResult.Ok,
            AISyncExecutionStage.Rejected => true,
            _ => false
        };
    }
}
