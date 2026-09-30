using System.Buffers.Binary;

namespace AIMP.Protocol;

// An execution snapshot, not a serialized Behaviour Tree or a WorldMutation.
// Fixed-size payloads carry no process addresses, ObjectRef pointers or BT state.
public static class AISyncCodec
{
    public const uint StateVersion = 2;
    public const int NativeStateBytes = 76;
    public const int NetworkStateBytes = 16 + NativeStateBytes;
    public const int MaximumActors = 16;

    public static ulong SessionCookie(Guid session)
    {
        Span<byte> bytes = stackalloc byte[16];
        session.TryWriteBytes(bytes);
        var cookie = BinaryPrimitives.ReadUInt64LittleEndian(bytes);
        return cookie == 0 ? 1UL : cookie;
    }

    public static void ValidateNative(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeStateBytes || U32(bytes, 0) != StateVersion || U32(bytes, 4) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == 0 || U32(bytes, 16) == 0
            || (U32(bytes, 20) == 0 && U32(bytes, 24) == 0) || U32(bytes, 32) == 0
            || U32(bytes, 36) == 0 || U32(bytes, 40) > 1 || U32(bytes, 44) > 3
            || U32(bytes, 60) > 8 || (U32(bytes, 72) & ~0x00070101U) != 0
            || U32(bytes, 28) is not (1 or 3 or 4 or 6 or 9))
            throw new ProtocolException("Invalid AISync execution snapshot v2.");
        for (var offset = 48; offset <= 56; offset += 4)
        {
            var value = Float(bytes, offset);
            if (!float.IsFinite(value) || Math.Abs(value) > 100000)
                throw new ProtocolException("Invalid AISync destination.");
        }
        for (var offset = 64; offset <= 68; offset += 4)
        {
            var value = Float(bytes, offset);
            if (!float.IsFinite(value) || value < 0 || value > 100)
                throw new ProtocolException("Invalid AISync movement parameter.");
        }
    }

    public static bool BelongsTo(ReadOnlySpan<byte> bytes, Guid session, int worldGeneration) =>
        bytes.Length == NativeStateBytes && session != Guid.Empty && worldGeneration > 0
        && BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == SessionCookie(session)
        && U32(bytes, 16) == (uint)worldGeneration;

    public static ulong ActorKey(ReadOnlySpan<byte> bytes) =>
        ((ulong)U32(bytes, 20) << 32) | U32(bytes, 24);

    public static byte[] EncodeNetwork(Guid session, ReadOnlySpan<byte> native)
    {
        ValidateNative(native);
        if (session == Guid.Empty || BinaryPrimitives.ReadUInt64LittleEndian(native[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync session identity mismatch.");
        var bytes = new byte[NetworkStateBytes];
        session.TryWriteBytes(bytes.AsSpan(0, 16));
        native.CopyTo(bytes.AsSpan(16));
        return bytes;
    }

    public static ReadOnlyMemory<byte> DecodeNetwork(ReadOnlyMemory<byte> bytes, Guid session)
    {
        if (bytes.Length != NetworkStateBytes || new Guid(bytes.Span[..16]) != session)
            throw new ProtocolException("AISync network session mismatch.");
        var native = bytes[16..];
        ValidateNative(native.Span);
        if (BinaryPrimitives.ReadUInt64LittleEndian(native.Span[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync cookie mismatch.");
        return native;
    }

    public static byte[] Configuration(uint role, int nativeGeneration, int worldGeneration, Guid session)
    {
        if (role > 3 || nativeGeneration < 0 || worldGeneration < 0)
            throw new ArgumentOutOfRangeException(nameof(role));
        if (role != 0 && (session == Guid.Empty ||
            (role != 3 && (nativeGeneration == 0 || worldGeneration == 0))))
            throw new ProtocolException("AISync configuration needs a live generation.");
        var bytes = new byte[24];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), role);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)nativeGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), (uint)worldGeneration);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), role == 0 ? 0 : SessionCookie(session));
        return bytes;
    }

    private static uint U32(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static float Float(ReadOnlySpan<byte> bytes, int offset) =>
        BitConverter.Int32BitsToSingle((int)U32(bytes, offset));
}
