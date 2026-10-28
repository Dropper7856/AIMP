using System.Buffers.Binary;
using System.Security.Cryptography;
using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class AISyncShadowTests
{
    private static readonly Guid Session = new("00112233-4455-6677-8899-aabbccddeeff");
    private static readonly Guid Consumer = new("11112233-4455-6677-8899-aabbccddeeff");
    private static readonly AISyncShadowStreamId Stream = new(Session, 7,
        new Guid("22112233-4455-6677-8899-aabbccddeeff"));

    internal static Task CodecRoundTripsAsync()
    {
        var journal = new AISyncShadowJournal(Stream);
        var open = journal.CreateOpen(Consumer);
        Equal(open, AISyncShadowCodec.DecodeOpen(AISyncShadowCodec.EncodeOpen(open)));
        for (ulong i = 1; i <= 16; i++) Equal(i, journal.Append(Native(i)));
        var data = journal.CreateData(Consumer, 1);
        var encoded = AISyncShadowCodec.EncodeData(data);
        Equal(AISyncShadowCodec.MaxDataBytes, encoded.Length);
        Check(encoded.Length < 8192);
        Bytes(encoded, AISyncShadowCodec.EncodeData(AISyncShadowCodec.DecodeData(encoded)));
        Equal(Stream.SessionGuid, new Guid(encoded.AsSpan(16, 16)));
        Equal(Stream.StreamGuid, new Guid(encoded.AsSpan(40, 16)));
        Equal(Consumer, new Guid(encoded.AsSpan(56, 16)));
        var previous = new byte[32];
        for (var i = 0; i < data.Observations.Length; i++)
        {
            // Independent concatenation checks the documented digest layout.
            var material = previous.Concat(BitConverter.GetBytes((ulong)i + 1)).Concat(data.Observations[i]).ToArray();
            previous = SHA256.HashData(material);
        }
        Bytes(previous, data.Digest);
        var receiver = new AISyncShadowReceiverWindow(open);
        Equal(AISyncShadowResult.Accepted, receiver.Receive(AISyncShadowCodec.DecodeData(encoded)));
        var receipt = receiver.CreateReceipt();
        Bytes(AISyncShadowCodec.EncodeReceipt(receipt), AISyncShadowCodec.EncodeReceipt(
            AISyncShadowCodec.DecodeReceipt(AISyncShadowCodec.EncodeReceipt(receipt))));
        Equal(AISyncShadowResult.Accepted, journal.AcceptReceipt(receipt));
        Equal(0UL, receipt.AppliedOrdinal);
        foreach (var reason in Enum.GetValues<AISyncShadowRecoveryReason>())
        {
            var recovery = receiver.CreateRecovery(reason);
            Equal(recovery, AISyncShadowCodec.DecodeRecovery(AISyncShadowCodec.EncodeRecovery(recovery)));
        }
        var raw = Native(100);
        Equal(216, raw.Length);
        Equal(0x4F544941U, BinaryPrimitives.ReadUInt32LittleEndian(raw));
        Bytes("AITO"u8.ToArray(), raw[..4]);
        Equal((ushort)216, BinaryPrimitives.ReadUInt16LittleEndian(raw.AsSpan(6)));
        Equal(100UL, BinaryPrimitives.ReadUInt64LittleEndian(raw.AsSpan(24)));
        Bytes(AISyncTransactionCodec.Encode(Value(100)), raw[56..]);
        BinaryPrimitives.WriteUInt64LittleEndian(raw.AsSpan(48), ulong.MaxValue);
        _ = AISyncShadowCodec.ValidateNativeObservation(raw, Stream); // loss is diagnostic, never completeness
        return Task.CompletedTask;
    }

    internal static Task MalformedPacketsAsync()
    {
        var journal = new AISyncShadowJournal(Stream);
        var open = journal.CreateOpen(Consumer);
        journal.Append(Native(1));
        var data = journal.CreateData(Consumer, 1);
        var receiver = new AISyncShadowReceiverWindow(open);
        receiver.Receive(data);
        var packets = new (byte[] Bytes, Action<byte[]> Decode)[]
        {
            (AISyncShadowCodec.EncodeOpen(open), b => AISyncShadowCodec.DecodeOpen(b)),
            (AISyncShadowCodec.EncodeData(data), b => AISyncShadowCodec.DecodeData(b)),
            (AISyncShadowCodec.EncodeReceipt(receiver.CreateReceipt()), b => AISyncShadowCodec.DecodeReceipt(b)),
            (AISyncShadowCodec.EncodeRecovery(receiver.CreateRecovery(AISyncShadowRecoveryReason.Gap)), b => AISyncShadowCodec.DecodeRecovery(b))
        };
        foreach (var (bytes, decode) in packets)
        {
            for (var length = 0; length < bytes.Length; length++)
            {
                var truncated = bytes[..length];
                Throws<ProtocolException>(() => decode(truncated));
            }
            Throws<ProtocolException>(() => decode(bytes.Concat(new byte[1]).ToArray()));
            foreach (var offset in Enumerable.Range(0, 16))
            {
                var malformed = bytes.ToArray();
                malformed[offset] ^= 0x80;
                Throws<ProtocolException>(() => decode(malformed));
            }
            foreach (var (offset, length) in new[] { (16, 16), (32, 8), (40, 16), (56, 16) })
            {
                var malformed = bytes.ToArray();
                malformed.AsSpan(offset, length).Clear();
                Throws<ProtocolException>(() => decode(malformed));
            }
        }
        var raw = Native(1);
        for (var length = 0; length < raw.Length; length++)
        {
            var truncated = raw[..length];
            Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(truncated));
        }
        Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(raw.Concat(new byte[1]).ToArray()));
        foreach (var offset in Enumerable.Range(0, 8))
        {
            var malformed = raw.ToArray();
            malformed[offset] ^= 0x80;
            Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(malformed));
        }
        foreach (var (offset, value) in new[] { (8, 2U), (12, 3U), (16, 0U), (16, 8U), (20, 8U) })
            RejectNative(WithU32(raw, offset, value));
        foreach (var offset in new[] { 24, 32 })
        {
            var malformed = raw.ToArray();
            malformed.AsSpan(offset, 8).Clear();
            RejectNative(malformed);
        }
        foreach (var flags in new[] { 0U, 16U, 32U, 48U | 64U, uint.MaxValue })
            RejectNative(WithU32(raw, 56 + 116, flags));
        foreach (var offset in new[] { 136, 140, 144, 148, 152, 156 })
            RejectNative(WithU32(raw, 56 + offset, 0x7FC00000));
        RejectNative(WithU32(raw, 56 + 104, 99));
        var badCookie = raw.ToArray(); badCookie[64] ^= 1;
        Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(badCookie, Stream));
        var badNamespace = raw.ToArray(); badNamespace[72] ^= 1;
        Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(badNamespace, Stream));
        Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(raw, Stream with { WorldGeneration = 8 }));
        Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(raw,
            Stream with { SessionGuid = new Guid("ff112233-4455-6677-8899-aabbccddeeff") }));
        var encoded = AISyncShadowCodec.EncodeData(data);
        foreach (var count in new[] { 0U, 17U, uint.MaxValue })
        {
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeData(WithU32(encoded, 80, count)));
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeDataScope(WithU32(encoded, 80, count)));
        }
        Throws<ProtocolException>(() => AISyncShadowCodec.DecodeData(WithU32(encoded, 84, 1)));
        Throws<ProtocolException>(() => AISyncShadowCodec.DecodeDataScope(WithU32(encoded, 84, 1)));
        for (var length = 0; length < encoded.Length; length++)
        {
            var truncated = encoded[..length];
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeDataScope(truncated));
        }
        foreach (var offset in Enumerable.Range(0, 16))
        {
            var malformed = encoded.ToArray(); malformed[offset] ^= 0x80;
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeDataScope(malformed));
        }
        foreach (var offset in new[] { 88, 120, AISyncShadowCodec.DataHeaderBytes + 48 })
        {
            var altered = encoded.ToArray(); altered[offset] ^= 1;
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeData(altered));
        }
        Throws<ProtocolException>(() => AISyncShadowCodec.EncodeData(data with { FirstOrdinal = ulong.MaxValue }));
        Throws<ProtocolException>(() => AISyncShadowCodec.EncodeData(data with { PreviousDigest = new byte[31] }));
        Throws<ProtocolException>(() => AISyncShadowCodec.EncodeData(data with { Digest = new byte[33] }));
        Throws<ProtocolException>(() => AISyncShadowCodec.EncodeReceipt(receiver.CreateReceipt() with { AppliedOrdinal = 1 }));
        Throws<ProtocolException>(() => AISyncShadowCodec.DecodeReceipt(
            WithU32(AISyncShadowCodec.EncodeReceipt(receiver.CreateReceipt()), 88, 1)));
        Throws<ProtocolException>(() => AISyncShadowCodec.DecodeRecovery(
            WithU32(packets[3].Bytes, 76, 1)));
        Throws<ProtocolException>(() => AISyncShadowCodec.DecodeRecovery(
            WithU32(packets[3].Bytes, 72, 6)));
        return Task.CompletedTask;
    }

    internal static Task JournalBoundsAndReceiptsAsync()
    {
        var journal = new AISyncShadowJournal(Stream, maxRecords: 2);
        var open = journal.CreateOpen(Consumer);
        var receiver = new AISyncShadowReceiverWindow(open);
        var raw = Native(1);
        journal.Append(raw);
        raw[48] ^= 1; // caller cannot change retained bytes
        Bytes(Native(1), journal.CreateData(Consumer, 1).Observations[0]);
        var packet = journal.CreateData(Consumer, 1);
        receiver.Receive(packet);
        packet.Observations[0][48] ^= 1;
        packet.Digest[0] ^= 1;
        Bytes(Native(1), journal.CreateData(Consumer, 1).Observations[0]);
        Equal(AISyncShadowResult.Accepted, journal.AcceptReceipt(receiver.CreateReceipt()));
        Equal(AISyncShadowResult.Duplicate, journal.AcceptReceipt(receiver.CreateReceipt()));
        var bad = receiver.CreateReceipt(); bad.Digest[0] ^= 1;
        Equal(AISyncShadowResult.RecoveryRequired, journal.AcceptReceipt(bad));
        Check(!journal.RecoveryRequired); // a hostile consumer cannot poison other peers
        Equal(AISyncShadowResult.Rejected, journal.AcceptReceipt(receiver.CreateReceipt() with { AppliedOrdinal = 1 }));
        Equal(AISyncShadowResult.Rejected, journal.AcceptReceipt(receiver.CreateReceipt() with { VerifiedOrdinal = 0 }));
        Equal(AISyncShadowResult.Rejected, journal.AcceptReceipt(receiver.CreateReceipt() with { ConsumerGuid = Guid.Empty }));
        Equal(AISyncShadowResult.Rejected, journal.AcceptReceipt(receiver.CreateReceipt() with { ReceivedOrdinal = 3, VerifiedOrdinal = 3 }));
        Equal(2UL, journal.Append(Native(2)));
        Equal(2, journal.Count);
        Equal(2L * AISyncShadowJournal.RetainedRecordBytes, journal.Bytes);
        Equal(1UL, journal.CreateOpen(Consumer).FirstOrdinal); // ACK never discards the baseline
        Throws<ProtocolException>(() => journal.Append(Native(3)));
        Check(journal.RecoveryRequired);
        Equal(AISyncShadowRecoveryReason.Overflow, journal.RecoveryReason);
        Equal(2, journal.Count);
        Throws<ProtocolException>(() => journal.CreateData(Consumer, 1));
        var bytesLimited = new AISyncShadowJournal(Stream,
            maxBytes: AISyncShadowJournal.RetainedRecordBytes - 1);
        Throws<ProtocolException>(() => bytesLimited.Append(Native(1)));
        Equal(0, bytesLimited.Count);
        Check(bytesLimited.RecoveryRequired);
        var consumers = new AISyncShadowJournal(Stream);
        for (var i = 0; i < AISyncShadowJournal.MaxConsumers; i++)
            consumers.CreateOpen(DeterministicGuid(i + 1));
        Throws<ProtocolException>(() => consumers.CreateOpen(DeterministicGuid(100)));
        Check(consumers.RemoveConsumer(DeterministicGuid(1)));
        consumers.CreateOpen(DeterministicGuid(100));
        Throws<ArgumentOutOfRangeException>(() => new AISyncShadowJournal(Stream, maxRecords: 32769));
        Throws<ArgumentOutOfRangeException>(() => new AISyncShadowJournal(Stream, maxBytes: 8 * 1024 * 1024 + 1));
        Throws<ArgumentOutOfRangeException>(() => new AISyncShadowReceiverWindow(open, maxRecords: 0));
        var edge = new AISyncShadowJournal(Stream, ulong.MaxValue - 1);
        Equal(ulong.MaxValue - 1, edge.Append(Native(1)));
        Throws<ProtocolException>(() => edge.Append(Native(2))); // never wraps the global ordinal
        return Task.CompletedTask;
    }

    internal static Task ReceiverRecoveryAsync()
    {
        var journal = new AISyncShadowJournal(Stream);
        var open = journal.CreateOpen(Consumer);
        for (ulong i = 1; i <= 20; i++) journal.Append(Native(i));
        var receiver = new AISyncShadowReceiverWindow(open);
        Equal(AISyncShadowResult.Accepted, receiver.Receive(journal.CreateData(Consumer, 1, 8)));
        Equal(AISyncShadowResult.Duplicate, receiver.Receive(journal.CreateData(Consumer, 1, 8)));
        Equal(AISyncShadowResult.Accepted, receiver.Receive(journal.CreateData(Consumer, 5, 8))); // overlap
        Equal(12UL, receiver.VerifiedOrdinal);
        Equal(AISyncShadowResult.RecoveryRequired, receiver.Receive(journal.CreateData(Consumer, 14, 1)));
        Equal(AISyncShadowRecoveryReason.Gap, receiver.RecoveryReason);
        Equal(12UL, receiver.VerifiedOrdinal);
        Equal(AISyncShadowResult.RecoveryRequired, receiver.Receive(journal.CreateData(Consumer, 13, 1)));
        var conflicting = new AISyncShadowReceiverWindow(open);
        conflicting.Receive(journal.CreateData(Consumer, 1, 1));
        var altered = journal.CreateData(Consumer, 1, 1);
        altered.Observations[0][48] ^= 1; // still valid, different loss count
        altered = altered with { Digest = AISyncShadowCodec.ComputeDigest(altered.PreviousDigest, 1, altered.Observations[0]) };
        Equal(AISyncShadowResult.RecoveryRequired, conflicting.Receive(altered));
        Equal(AISyncShadowRecoveryReason.ConflictingDuplicate, conflicting.RecoveryReason);
        Equal(1UL, conflicting.VerifiedOrdinal);
        var limited = new AISyncShadowReceiverWindow(open, maxRecords: 1);
        Equal(AISyncShadowResult.RecoveryRequired, limited.Receive(journal.CreateData(Consumer, 1, 2)));
        Equal(0, limited.Count); // whole batch is rejected atomically
        Equal(AISyncShadowRecoveryReason.Overflow, limited.RecoveryReason);
        var bytesLimited = new AISyncShadowReceiverWindow(open,
            maxBytes: AISyncShadowJournal.RetainedRecordBytes);
        Equal(AISyncShadowResult.Accepted, bytesLimited.Receive(journal.CreateData(Consumer, 1, 1)));
        Equal(AISyncShadowResult.RecoveryRequired, bytesLimited.Receive(journal.CreateData(Consumer, 2, 1)));
        Equal(1UL, bytesLimited.VerifiedOrdinal);
        var corrupt = new AISyncShadowReceiverWindow(open);
        var badDigest = journal.CreateData(Consumer, 1, 1); badDigest.Digest[0] ^= 1;
        Equal(AISyncShadowResult.RecoveryRequired, corrupt.Receive(badDigest));
        Equal(0UL, corrupt.VerifiedOrdinal);
        Equal(0UL, corrupt.AppliedOrdinal);
        return Task.CompletedTask;
    }

    internal static Task ReconnectAndEpochsAsync()
    {
        var first = new AISyncShadowJournal(Stream);
        var old = new AISyncShadowReceiverWindow(first.CreateOpen(Consumer));
        first.Append(Native(1));
        old.Receive(first.CreateData(Consumer, 1));
        var receipt = old.CreateReceipt();
        first.RemoveConsumer(Consumer);
        Equal(AISyncShadowResult.Rejected, first.AcceptReceipt(receipt));
        var nextConsumer = DeterministicGuid(47);
        var nextStream = Stream with { StreamGuid = DeterministicGuid(48) };
        var next = new AISyncShadowJournal(nextStream, first.LastOrdinal + 1);
        var fresh = new AISyncShadowReceiverWindow(next.CreateOpen(nextConsumer));
        Equal(0, fresh.Count);
        Equal(2UL, fresh.Open.FirstOrdinal);
        Equal(AISyncShadowResult.Rejected, next.AcceptReceipt(receipt));
        first.CreateOpen(Consumer);
        Equal(AISyncShadowResult.Rejected, fresh.Receive(first.CreateData(Consumer, 1)));
        var stale = AISyncShadowCodec.EncodeData(first.CreateData(Consumer, 1));
        foreach (var offset in new[] { 120, AISyncShadowCodec.DataHeaderBytes })
        {
            var malformed = stale.ToArray(); malformed[offset] ^= 1;
            var scope = AISyncShadowCodec.DecodeDataScope(malformed);
            Equal(Stream, scope.Stream);
            Equal(Consumer, scope.ConsumerGuid);
            Check(scope.Stream != fresh.Stream && scope.ConsumerGuid != fresh.ConsumerGuid);
            Throws<ProtocolException>(() => AISyncShadowCodec.DecodeData(malformed));
            Equal(0, fresh.Count);
            Check(!fresh.RecoveryRequired);
        }
        next.Append(Native(2));
        var packet = next.CreateData(nextConsumer, 2);
        Equal(AISyncShadowResult.Rejected, fresh.Receive(packet with { ConsumerGuid = Consumer }));
        Equal(AISyncShadowResult.Rejected, fresh.Receive(packet with { Stream = nextStream with { WorldGeneration = 8 } }));
        // Same native cookie, different full session GUID: outer binding still rejects it.
        var collisionSession = new Guid("00112233-4455-6677-8899-aabbccddeefe");
        Equal(AISyncCodec.SessionCookie(Session), AISyncCodec.SessionCookie(collisionSession));
        Equal(AISyncShadowResult.Rejected, fresh.Receive(packet with { Stream = nextStream with { SessionGuid = collisionSession } }));
        Equal(AISyncShadowResult.Accepted, fresh.Receive(packet));
        Equal(AISyncShadowResult.Accepted, next.AcceptReceipt(fresh.CreateReceipt()));
        Bytes(new byte[32], packet.PreviousDigest); // empty baseline, no native mid-flight checkpoint
        return Task.CompletedTask;
    }

    internal static Task GeneratedObservationsAsync()
    {
        // 12,000 deterministic observations across all canonical opcodes, native
        // operations, coverage reasons and both native streams, without native calls.
        var journal = new AISyncShadowJournal(Stream);
        var open = journal.CreateOpen(Consumer);
        var receiver = new AISyncShadowReceiverWindow(open);
        const int records = 12000;
        for (ulong i = 1; i <= records; i++) Equal(i, journal.Append(Native(i)));
        for (ulong first = 1; first <= records; first += 16)
        {
            var data = journal.CreateData(Consumer, first);
            var wire = AISyncShadowCodec.EncodeData(data);
            var decoded = AISyncShadowCodec.DecodeData(wire);
            Bytes(wire, AISyncShadowCodec.EncodeData(decoded));
            for (var i = 0; i < decoded.Observations.Length; i++)
                Bytes(Native(first + (ulong)i), decoded.Observations[i]);
            Equal(AISyncShadowResult.Accepted, receiver.Receive(decoded));
            Equal(AISyncShadowResult.Duplicate, receiver.Receive(decoded));
            var receipt = AISyncShadowCodec.DecodeReceipt(AISyncShadowCodec.EncodeReceipt(receiver.CreateReceipt()));
            Equal(AISyncShadowResult.Accepted, journal.AcceptReceipt(receipt));
        }
        Equal(records, receiver.Count);
        Equal((ulong)records, receiver.VerifiedOrdinal);
        Equal(0UL, receiver.AppliedOrdinal);
        Check(!receiver.RecoveryRequired && !journal.RecoveryRequired);
        // Late join receives identical retained bytes and an independently verified chain.
        var lateConsumer = DeterministicGuid(88);
        var late = new AISyncShadowReceiverWindow(journal.CreateOpen(lateConsumer));
        for (ulong first = 1; first <= records; first += 16)
            Equal(AISyncShadowResult.Accepted, late.Receive(journal.CreateData(lateConsumer, first)));
        Bytes(receiver.CreateReceipt().Digest, late.CreateReceipt().Digest);
        Equal(AISyncShadowResult.Accepted, journal.AcceptReceipt(late.CreateReceipt()));
        var oldReceipt = new AISyncShadowReceiverWindow(open).CreateReceipt();
        Equal(AISyncShadowResult.Rejected, journal.AcceptReceipt(oldReceipt));
        return Task.CompletedTask;
    }

    private static AISyncTransaction Value(ulong ordinal)
    {
        Span<byte> session = stackalloc byte[16];
        BinaryPrimitives.WriteUInt64LittleEndian(session, AISyncCodec.SessionCookie(Session));
        "AIMPLOCL"u8.CopyTo(session[8..]);
        return new AISyncTransaction
        {
            Session = new Guid(session), AuthorityEpoch = 1, WorldEpoch = Stream.WorldGeneration,
            ConsumerEpoch = 1, OwnerEpoch = 5, EntityA = (uint)(ordinal % 128 + 1),
            EntityB = 4, Birth = 1, CharacterClass = 2, CommandId = ordinal, ParentCommandId = 0,
            GlobalSequence = ordinal, OwnerSequence = ordinal,
            Opcode = (AISyncTransactionOpcode)(ordinal % 9 + 1), Capability = AISyncTransactionCapability.Movement,
            ParameterRevision = 1, Flags = AISyncTransactionFlags.Incomplete | AISyncTransactionFlags.ObservationOnly
                | (AISyncTransactionFlags)(ordinal % 16),
            NativeKind = (uint)(ordinal % 256), Gait = (uint)(ordinal % 3), Mode = (uint)(ordinal % 4),
            NativeStatus = (uint)(ordinal % 2), GoalX = (float)(ordinal * 0.25),
            GoalY = BitConverter.UInt32BitsToSingle(0x80000000), GoalZ = -3,
            Distance = 2, Stop = 1, Aux = -1
        };
    }

    private static byte[] Native(ulong ordinal)
    {
        var bytes = new byte[216];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 0x4F544941);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), 216);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)(ordinal % 2));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), (uint)(ordinal % 3));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), (uint)(ordinal % 7 + 1));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(20), (uint)(ordinal % 8));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), ordinal);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), ordinal / 100 + 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(40), ordinal % 3);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(48), ordinal / 999);
        AISyncTransactionCodec.Encode(Value(ordinal), bytes.AsSpan(56));
        return bytes;
    }

    private static byte[] WithU32(byte[] source, int offset, uint value)
    {
        var bytes = source.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        return bytes;
    }

    private static Guid DeterministicGuid(int value) => new(value, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    private static void RejectNative(byte[] bytes) => Throws<ProtocolException>(() => AISyncShadowCodec.ValidateNativeObservation(bytes));
    private static void Check(bool condition) { if (!condition) throw new InvalidOperationException("AISync shadow assertion failed."); }
    private static void Equal<T>(T expected, T actual) where T : notnull => Check(EqualityComparer<T>.Default.Equals(expected, actual));
    private static void Bytes(byte[] expected, byte[] actual) => Check(expected.AsSpan().SequenceEqual(actual));
    private static void Throws<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return; }
        throw new InvalidOperationException($"Expected {typeof(T).Name}.");
    }
}
