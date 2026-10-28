namespace AIMP.Protocol;

/// <summary>
/// Bounded observation history, with no native execution and no silent eviction.
/// Serialize access externally (the lobby owns FIFO scheduling and authentication).
/// A new binding/reconnect needs a new stream GUID and empty baseline; FirstOrdinal
/// may continue a host-wide ordinal counter, but never resumes native execution.
/// </summary>
public sealed class AISyncShadowJournal
{
    public const int DefaultMaxRecords = 32768;
    public const long DefaultMaxBytes = 8 * 1024 * 1024;
    public const int MaxConsumers = 32;
    // Raw payload, chain digest, ordinal and worst-case single-record frame header.
    // This deliberately accounts for more than just the 160-byte canonical body.
    public const int RetainedRecordBytes = AISyncShadowCodec.NativeObservationBytes
        + AISyncShadowCodec.DigestBytes + 8 + AISyncShadowCodec.DataHeaderBytes;
    private readonly List<Entry> _entries = new();
    private readonly Dictionary<Guid, ulong> _receipts = new();
    private readonly int _maxRecords;
    private readonly long _maxBytes;
    private sealed record Entry(byte[] Raw, byte[] Digest);

    public AISyncShadowJournal(AISyncShadowStreamId stream, ulong firstOrdinal = 1,
        int maxRecords = DefaultMaxRecords, long maxBytes = DefaultMaxBytes)
    {
        AISyncShadowCodec.ValidateStream(stream);
        ValidateLimits(maxRecords, maxBytes);
        if (firstOrdinal == 0 || firstOrdinal == ulong.MaxValue)
            throw new ProtocolException("Invalid AISync shadow initial ordinal.");
        Stream = stream;
        FirstOrdinal = firstOrdinal;
        _maxRecords = maxRecords;
        _maxBytes = maxBytes;
    }

    public AISyncShadowStreamId Stream { get; }
    public ulong FirstOrdinal { get; }
    public ulong LastOrdinal => FirstOrdinal - 1 + (ulong)Count;
    public int Count => _entries.Count;
    public long Bytes => (long)Count * RetainedRecordBytes;
    public bool RecoveryRequired { get; private set; }
    public AISyncShadowRecoveryReason RecoveryReason { get; private set; } = AISyncShadowRecoveryReason.RestartRequired;

    public ulong Append(ReadOnlySpan<byte> observation)
    {
        AISyncShadowCodec.ValidateNativeObservation(observation, Stream);
        if (RecoveryRequired) throw new ProtocolException("AISync shadow stream requires restart.");
        if (Count >= _maxRecords || Bytes > _maxBytes - RetainedRecordBytes || LastOrdinal >= ulong.MaxValue - 1)
        {
            RecoveryRequired = true;
            RecoveryReason = AISyncShadowRecoveryReason.Overflow;
            throw new ProtocolException("AISync shadow journal overflow; restart with a new stream.");
        }
        var ordinal = LastOrdinal + 1;
        var previous = Count == 0 ? new byte[AISyncShadowCodec.DigestBytes] : _entries[^1].Digest;
        _entries.Add(new Entry(observation.ToArray(), AISyncShadowCodec.ComputeDigest(previous, ordinal, observation)));
        return ordinal;
    }

    public AISyncShadowOpen CreateOpen(Guid consumer)
    {
        var open = new AISyncShadowOpen(Stream, consumer, FirstOrdinal);
        _ = AISyncShadowCodec.EncodeOpen(open);
        if (RecoveryRequired) throw new ProtocolException("AISync shadow baseline requires a new stream.");
        if (!_receipts.ContainsKey(consumer))
        {
            if (_receipts.Count >= MaxConsumers) throw new ProtocolException("AISync shadow consumer limit exceeded.");
            _receipts.Add(consumer, FirstOrdinal - 1);
        }
        return open;
    }

    /// <summary>Retire a disconnected consumer; its subsequent receipts are rejected.</summary>
    public bool RemoveConsumer(Guid consumer) => _receipts.Remove(consumer);

    public AISyncShadowData CreateData(Guid consumer, ulong firstOrdinal, int maxRecords = AISyncShadowCodec.MaxBatchRecords)
    {
        if (RecoveryRequired || !_receipts.ContainsKey(consumer)
            || maxRecords is < 1 or > AISyncShadowCodec.MaxBatchRecords
            || firstOrdinal < FirstOrdinal || firstOrdinal > LastOrdinal)
            throw new ProtocolException("AISync shadow data unavailable; bind consumer or restart stream.");
        var index = checked((int)(firstOrdinal - FirstOrdinal));
        var count = Math.Min(maxRecords, Count - index);
        var observations = new byte[count][];
        for (var i = 0; i < count; i++) observations[i] = _entries[index + i].Raw.ToArray();
        return new AISyncShadowData(Stream, consumer, firstOrdinal,
            index == 0 ? new byte[AISyncShadowCodec.DigestBytes] : _entries[index - 1].Digest.ToArray(),
            observations, _entries[index + count - 1].Digest.ToArray());
    }

    public AISyncShadowResult AcceptReceipt(AISyncShadowReceipt receipt)
    {
        if (receipt.Stream != Stream || !_receipts.TryGetValue(receipt.ConsumerGuid, out var prior))
            return AISyncShadowResult.Rejected;
        try { _ = AISyncShadowCodec.EncodeReceipt(receipt); }
        catch (ProtocolException) { return AISyncShadowResult.Rejected; }
        if (receipt.VerifiedOrdinal < prior || receipt.VerifiedOrdinal > LastOrdinal
            || receipt.VerifiedOrdinal < FirstOrdinal - 1)
            return AISyncShadowResult.Rejected;
        var expected = receipt.VerifiedOrdinal == FirstOrdinal - 1
            ? new byte[AISyncShadowCodec.DigestBytes]
            : _entries[checked((int)(receipt.VerifiedOrdinal - FirstOrdinal))].Digest;
        if (!expected.AsSpan().SequenceEqual(receipt.Digest))
            return AISyncShadowResult.RecoveryRequired;
        _receipts[receipt.ConsumerGuid] = receipt.VerifiedOrdinal;
        return receipt.VerifiedOrdinal == prior ? AISyncShadowResult.Duplicate : AISyncShadowResult.Accepted;
    }

    internal static void ValidateLimits(int records, long bytes)
    {
        if (records is < 1 or > DefaultMaxRecords || bytes is < 1 or > DefaultMaxBytes)
            throw new ArgumentOutOfRangeException(nameof(records), "AISync shadow limits must be within the default hard caps.");
    }
}

/// <summary>
/// Synchronous, atomic batch verification, bounded by the same caps as the sender.
/// Keeps exact bytes for retained duplicate checks. Any gap/conflict/overflow fences
/// this window until its owner constructs a new one for a fresh stream GUID.
/// </summary>
public sealed class AISyncShadowReceiverWindow
{
    private readonly List<Entry> _entries = new();
    private readonly int _maxRecords;
    private readonly long _maxBytes;
    private sealed record Entry(byte[] Raw, byte[] Digest);

    public AISyncShadowReceiverWindow(AISyncShadowOpen open,
        int maxRecords = AISyncShadowJournal.DefaultMaxRecords, long maxBytes = AISyncShadowJournal.DefaultMaxBytes)
    {
        _ = AISyncShadowCodec.EncodeOpen(open);
        AISyncShadowJournal.ValidateLimits(maxRecords, maxBytes);
        Open = open;
        _maxRecords = maxRecords;
        _maxBytes = maxBytes;
    }

    public AISyncShadowOpen Open { get; }
    public AISyncShadowStreamId Stream => Open.Stream;
    public Guid ConsumerGuid => Open.ConsumerGuid;
    public int Count => _entries.Count;
    public long Bytes => (long)Count * AISyncShadowJournal.RetainedRecordBytes;
    public ulong ReceivedOrdinal => Open.FirstOrdinal - 1 + (ulong)Count;
    public ulong VerifiedOrdinal => ReceivedOrdinal;
    public ulong AppliedOrdinal => 0;
    public bool RecoveryRequired { get; private set; }
    public AISyncShadowRecoveryReason RecoveryReason { get; private set; } = AISyncShadowRecoveryReason.RestartRequired;

    public AISyncShadowResult Receive(AISyncShadowData data)
    {
        if (data.Stream != Stream || data.ConsumerGuid != ConsumerGuid) return AISyncShadowResult.Rejected;
        if (RecoveryRequired) return AISyncShadowResult.RecoveryRequired;
        try { AISyncShadowCodec.ValidateData(data); }
        catch (ProtocolException) { return Fail(AISyncShadowRecoveryReason.DigestMismatch); }
        if (data.FirstOrdinal < Open.FirstOrdinal || data.FirstOrdinal > ReceivedOrdinal + 1)
            return Fail(AISyncShadowRecoveryReason.Gap);
        var start = checked((int)(data.FirstOrdinal - Open.FirstOrdinal));
        var previous = start == 0 ? new byte[AISyncShadowCodec.DigestBytes] : _entries[start - 1].Digest;
        if (!previous.AsSpan().SequenceEqual(data.PreviousDigest)) return Fail(AISyncShadowRecoveryReason.DigestMismatch);
        var duplicateCount = Math.Min(Count - start, data.Observations.Length);
        for (var i = 0; i < duplicateCount; i++)
            if (!_entries[start + i].Raw.AsSpan().SequenceEqual(data.Observations[i]))
                return Fail(AISyncShadowRecoveryReason.ConflictingDuplicate);
        var added = data.Observations.Length - duplicateCount;
        if (added > _maxRecords - Count || (long)added * AISyncShadowJournal.RetainedRecordBytes > _maxBytes - Bytes)
            return Fail(AISyncShadowRecoveryReason.Overflow);
        // Validation above must finish before publishing any cumulative progress.
        var digest = duplicateCount == 0 ? previous : _entries[start + duplicateCount - 1].Digest;
        for (var i = duplicateCount; i < data.Observations.Length; i++)
        {
            digest = AISyncShadowCodec.ComputeDigest(digest, data.FirstOrdinal + (ulong)i, data.Observations[i]);
            _entries.Add(new Entry(data.Observations[i].ToArray(), digest));
        }
        return added == 0 ? AISyncShadowResult.Duplicate : AISyncShadowResult.Accepted;
    }

    public AISyncShadowReceipt CreateReceipt() => new(Stream, ConsumerGuid,
        ReceivedOrdinal, VerifiedOrdinal, 0,
        Count == 0 ? new byte[AISyncShadowCodec.DigestBytes] : _entries[^1].Digest.ToArray());

    public AISyncShadowRecovery CreateRecovery(AISyncShadowRecoveryReason reason) =>
        new(Stream, ConsumerGuid, reason, ReceivedOrdinal + 1);

    private AISyncShadowResult Fail(AISyncShadowRecoveryReason reason)
    {
        RecoveryRequired = true;
        RecoveryReason = reason;
        return AISyncShadowResult.RecoveryRequired;
    }
}
