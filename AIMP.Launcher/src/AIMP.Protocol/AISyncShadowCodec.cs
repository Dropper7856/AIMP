using System.Buffers.Binary;
using System.Security.Cryptography;

namespace AIMP.Protocol;

public readonly record struct AISyncShadowStreamId(Guid SessionGuid, ulong WorldGeneration, Guid StreamGuid);
public sealed record AISyncShadowOpen(AISyncShadowStreamId Stream, Guid ConsumerGuid, ulong FirstOrdinal);
public sealed record AISyncShadowData(AISyncShadowStreamId Stream, Guid ConsumerGuid, ulong FirstOrdinal,
    byte[] PreviousDigest, byte[][] Observations, byte[] Digest);
public sealed record AISyncShadowReceipt(AISyncShadowStreamId Stream, Guid ConsumerGuid,
    ulong ReceivedOrdinal, ulong VerifiedOrdinal, ulong AppliedOrdinal, byte[] Digest);
public enum AISyncShadowRecoveryReason : uint
{
    Gap = 1, ConflictingDuplicate, Overflow, DigestMismatch, RestartRequired
}
public sealed record AISyncShadowRecovery(AISyncShadowStreamId Stream, Guid ConsumerGuid,
    AISyncShadowRecoveryReason Reason, ulong ExpectedOrdinal);
public enum AISyncShadowResult { Accepted, Duplicate, Rejected, RecoveryRequired }

/// <summary>
/// Observation-only transport. GUIDs use .NET byte order. The 72-byte common
/// header is magic/version/kind/size/reserved, full session/world/stream/consumer.
/// No native command, pointer or applied acknowledgement is represented here.
/// </summary>
public static class AISyncShadowCodec
{
    public const uint NativeMagic = 0x4F544941;
    public const uint Magic = 0x48534941; // AISH
    public const ushort Version = 1;
    public const int NativeObservationBytes = 216;
    public const int MaxBatchRecords = 16;
    public const int DigestBytes = 32;
    public const int HeaderBytes = 72;
    public const int OpenBytes = 80;
    public const int DataHeaderBytes = 152;
    public const int ReceiptBytes = 128;
    public const int RecoveryBytes = 88;
    public const int MaxDataBytes = DataHeaderBytes + MaxBatchRecords * NativeObservationBytes;

    public static AISyncTransaction ValidateNativeObservation(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeObservationBytes || U32(bytes, 0) != NativeMagic
            || U16(bytes, 4) != Version || U16(bytes, 6) != NativeObservationBytes
            || U32(bytes, 8) > 1 || U32(bytes, 12) > 2
            || U32(bytes, 16) is < 1 or > 7 || U32(bytes, 20) > 7
            || U64(bytes, 24) == 0 || U64(bytes, 32) == 0)
            throw new ProtocolException("Invalid AISync native observation layout or metadata.");
        var canonical = AISyncTransactionCodec.Decode(bytes[56..]);
        const AISyncTransactionFlags required = AISyncTransactionFlags.Incomplete | AISyncTransactionFlags.ObservationOnly;
        if ((canonical.Flags & required) != required)
            throw new ProtocolException("AISync shadow observations must remain incomplete and observation-only.");
        return canonical;
    }

    public static AISyncTransaction ValidateNativeObservation(ReadOnlySpan<byte> bytes, AISyncShadowStreamId stream)
    {
        ValidateStream(stream);
        var canonical = ValidateNativeObservation(bytes);
        var localSession = bytes.Slice(64, 16);
        if (canonical.WorldEpoch != stream.WorldGeneration
            || U64(localSession, 0) != AISyncCodec.SessionCookie(stream.SessionGuid)
            || !localSession[8..].SequenceEqual("AIMPLOCL"u8))
            throw new ProtocolException("AISync shadow native cookie, namespace or world mismatch.");
        return canonical;
    }

    public static byte[] EncodeOpen(AISyncShadowOpen value)
    {
        if (value.FirstOrdinal == 0 || value.FirstOrdinal == ulong.MaxValue)
            throw new ProtocolException("Invalid AISync shadow baseline.");
        var bytes = Header(value.Stream, value.ConsumerGuid, 1, OpenBytes);
        W64(bytes, 72, value.FirstOrdinal);
        return bytes;
    }

    public static AISyncShadowOpen DecodeOpen(ReadOnlySpan<byte> bytes)
    {
        var (stream, consumer) = ReadHeader(bytes, 1, OpenBytes);
        var value = new AISyncShadowOpen(stream, consumer, U64(bytes, 72));
        _ = EncodeOpen(value);
        return value;
    }

    public static byte[] EncodeData(AISyncShadowData value)
    {
        ValidateData(value);
        var bytes = Header(value.Stream, value.ConsumerGuid, 2,
            DataHeaderBytes + value.Observations.Length * NativeObservationBytes);
        W64(bytes, 72, value.FirstOrdinal);
        W32(bytes, 80, (uint)value.Observations.Length);
        value.PreviousDigest.CopyTo(bytes, 88);
        value.Digest.CopyTo(bytes, 120);
        for (var i = 0; i < value.Observations.Length; i++)
            value.Observations[i].CopyTo(bytes, DataHeaderBytes + i * NativeObservationBytes);
        return bytes;
    }

    /// <summary>
    /// Inspect the bounded data header before deciding whether this packet belongs
    /// to the current receiver. Does not validate native bodies or digests and does
    /// not establish receipt/verification; current-scope packets still need DecodeData.
    /// </summary>
    public static (AISyncShadowStreamId Stream, Guid ConsumerGuid) DecodeDataScope(ReadOnlySpan<byte> bytes)
    {
        // Check all counts and lengths before allocating even the observation array.
        if (bytes.Length < DataHeaderBytes || bytes.Length > MaxDataBytes)
            throw new ProtocolException("Invalid AISync shadow data length.");
        var count = U32(bytes, 80);
        if (count is < 1 or > MaxBatchRecords || U32(bytes, 84) != 0
            || bytes.Length != DataHeaderBytes + count * NativeObservationBytes)
            throw new ProtocolException("Invalid AISync shadow batch count or reserved bits.");
        return ReadHeader(bytes, 2, bytes.Length);
    }

    public static AISyncShadowData DecodeData(ReadOnlySpan<byte> bytes)
    {
        var (stream, consumer) = DecodeDataScope(bytes);
        var count = U32(bytes, 80);
        var observations = new byte[(int)count][];
        for (var i = 0; i < observations.Length; i++)
        {
            var raw = bytes.Slice(DataHeaderBytes + i * NativeObservationBytes, NativeObservationBytes);
            ValidateNativeObservation(raw, stream);
            observations[i] = raw.ToArray();
        }
        var value = new AISyncShadowData(stream, consumer, U64(bytes, 72),
            bytes.Slice(88, DigestBytes).ToArray(), observations, bytes.Slice(120, DigestBytes).ToArray());
        ValidateData(value);
        return value;
    }

    public static byte[] EncodeReceipt(AISyncShadowReceipt value)
    {
        if (value.AppliedOrdinal != 0 || value.VerifiedOrdinal != value.ReceivedOrdinal)
            throw new ProtocolException("AISync shadow receipts verify bytes only; applied must be zero.");
        ValidateDigest(value.Digest);
        var bytes = Header(value.Stream, value.ConsumerGuid, 3, ReceiptBytes);
        W64(bytes, 72, value.ReceivedOrdinal);
        W64(bytes, 80, value.VerifiedOrdinal);
        W64(bytes, 88, value.AppliedOrdinal);
        value.Digest.CopyTo(bytes, 96);
        return bytes;
    }

    public static AISyncShadowReceipt DecodeReceipt(ReadOnlySpan<byte> bytes)
    {
        var (stream, consumer) = ReadHeader(bytes, 3, ReceiptBytes);
        var value = new AISyncShadowReceipt(stream, consumer, U64(bytes, 72), U64(bytes, 80),
            U64(bytes, 88), bytes.Slice(96, DigestBytes).ToArray());
        _ = EncodeReceipt(value);
        return value;
    }

    public static byte[] EncodeRecovery(AISyncShadowRecovery value)
    {
        if (value.Reason is < AISyncShadowRecoveryReason.Gap or > AISyncShadowRecoveryReason.RestartRequired
            || value.ExpectedOrdinal == 0)
            throw new ProtocolException("Invalid AISync shadow recovery reason or ordinal.");
        var bytes = Header(value.Stream, value.ConsumerGuid, 4, RecoveryBytes);
        W32(bytes, 72, (uint)value.Reason);
        W64(bytes, 80, value.ExpectedOrdinal);
        return bytes;
    }

    public static AISyncShadowRecovery DecodeRecovery(ReadOnlySpan<byte> bytes)
    {
        var (stream, consumer) = ReadHeader(bytes, 4, RecoveryBytes);
        if (U32(bytes, 76) != 0) throw new ProtocolException("Unknown AISync shadow recovery flags.");
        var value = new AISyncShadowRecovery(stream, consumer, (AISyncShadowRecoveryReason)U32(bytes, 72), U64(bytes, 80));
        _ = EncodeRecovery(value);
        return value;
    }

    /// <summary>SHA256(previous || ordinalLE || exact raw216). Consumer is not part of shared history.</summary>
    public static byte[] ComputeDigest(ReadOnlySpan<byte> previous, ulong ordinal, ReadOnlySpan<byte> observation)
    {
        if (previous.Length != DigestBytes || observation.Length != NativeObservationBytes || ordinal == 0)
            throw new ProtocolException("Invalid AISync shadow digest input.");
        Span<byte> input = stackalloc byte[DigestBytes + 8 + NativeObservationBytes];
        previous.CopyTo(input);
        BinaryPrimitives.WriteUInt64LittleEndian(input[DigestBytes..], ordinal);
        observation.CopyTo(input[(DigestBytes + 8)..]);
        return SHA256.HashData(input);
    }

    internal static void ValidateData(AISyncShadowData value)
    {
        ValidateStream(value.Stream);
        if (value.ConsumerGuid == Guid.Empty || value.FirstOrdinal == 0 || value.Observations is null
            || value.Observations.Length is < 1 or > MaxBatchRecords
            || value.FirstOrdinal > ulong.MaxValue - (ulong)value.Observations.Length)
            throw new ProtocolException("Invalid AISync shadow data identity or bounds.");
        ValidateDigest(value.PreviousDigest);
        ValidateDigest(value.Digest);
        var digest = value.PreviousDigest;
        for (var i = 0; i < value.Observations.Length; i++)
        {
            ValidateNativeObservation(value.Observations[i], value.Stream);
            digest = ComputeDigest(digest, value.FirstOrdinal + (ulong)i, value.Observations[i]);
        }
        if (!digest.AsSpan().SequenceEqual(value.Digest))
            throw new ProtocolException("AISync shadow batch digest mismatch.");
    }

    internal static void ValidateStream(AISyncShadowStreamId stream)
    {
        if (stream.SessionGuid == Guid.Empty || stream.WorldGeneration == 0 || stream.StreamGuid == Guid.Empty)
            throw new ProtocolException("AISync shadow requires a full session, world and fresh stream identity.");
    }

    private static void ValidateDigest(byte[]? digest)
    {
        if (digest is null || digest.Length != DigestBytes) throw new ProtocolException("Invalid AISync shadow digest length.");
    }

    private static byte[] Header(AISyncShadowStreamId stream, Guid consumer, ushort kind, int length)
    {
        ValidateStream(stream);
        if (consumer == Guid.Empty) throw new ProtocolException("AISync shadow requires a consumer GUID.");
        var bytes = new byte[length];
        W32(bytes, 0, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), Version);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), kind);
        W32(bytes, 8, (uint)length);
        stream.SessionGuid.TryWriteBytes(bytes.AsSpan(16, 16));
        W64(bytes, 32, stream.WorldGeneration);
        stream.StreamGuid.TryWriteBytes(bytes.AsSpan(40, 16));
        consumer.TryWriteBytes(bytes.AsSpan(56, 16));
        return bytes;
    }

    private static (AISyncShadowStreamId, Guid) ReadHeader(ReadOnlySpan<byte> bytes, ushort kind, int length)
    {
        if (bytes.Length != length || bytes.Length < HeaderBytes || U32(bytes, 0) != Magic
            || U16(bytes, 4) != Version || U16(bytes, 6) != kind || U32(bytes, 8) != length || U32(bytes, 12) != 0)
            throw new ProtocolException("Invalid AISync shadow header, length or flags.");
        var stream = new AISyncShadowStreamId(new Guid(bytes.Slice(16, 16)), U64(bytes, 32), new Guid(bytes.Slice(40, 16)));
        ValidateStream(stream);
        var consumer = new Guid(bytes.Slice(56, 16));
        if (consumer == Guid.Empty) throw new ProtocolException("Invalid AISync shadow consumer.");
        return (stream, consumer);
    }

    private static ushort U16(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(bytes[offset..]);
    private static uint U32(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static ulong U64(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt64LittleEndian(bytes[offset..]);
    private static void W32(Span<byte> bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], value);
    private static void W64(Span<byte> bytes, int offset, ulong value) => BinaryPrimitives.WriteUInt64LittleEndian(bytes[offset..], value);
}
