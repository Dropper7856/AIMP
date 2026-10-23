using System.Buffers.Binary;

namespace AIMP.Protocol;

// Exact native release/placement plus Host lifetime. The carrier contains no
// inventory slot, native address or inferred last-selected weapon.
public static class ThrowableSyncCodec
{
    public const int EventBytes = 272;
    public const int ConfigurationBytes = 32;
    public static ulong Peer(ReadOnlySpan<byte> data) => BinaryPrimitives.ReadUInt64LittleEndian(data[24..]);
    public static ulong Sequence(ReadOnlySpan<byte> data) => BinaryPrimitives.ReadUInt64LittleEndian(data[32..]);
    public static uint Operation(ReadOnlySpan<byte> data) => BinaryPrimitives.ReadUInt32LittleEndian(data[12..]);
    public static int Generation(ReadOnlySpan<byte> data) => unchecked((int)BinaryPrimitives.ReadUInt32LittleEndian(data[8..]));
    public static uint NativeGeneration(ReadOnlySpan<byte> data) => BinaryPrimitives.ReadUInt32LittleEndian(data[4..]);
    public static ulong Cookie(ReadOnlySpan<byte> data) => BinaryPrimitives.ReadUInt64LittleEndian(data[16..]);

    public static void Validate(ReadOnlySpan<byte> data)
    {
        if (data.Length != EventBytes || BinaryPrimitives.ReadUInt32LittleEndian(data) != 1
            || NativeGeneration(data) == 0 || Generation(data) < 1 || Cookie(data) == 0
            || Peer(data) == 0 || Sequence(data) == 0 || Operation(data) is not (1 or 2)
            || BinaryPrimitives.ReadUInt32LittleEndian(data[40..]) > 1
            || BinaryPrimitives.ReadUInt32LittleEndian(data[44..]) != 0)
            throw new ProtocolException("Invalid throwable lifetime carrier.");
        for (var offset = 48; offset < 112; offset += 4)
        {
            var value = BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(data[offset..]));
            var limit = offset is >= 96 and < 108 ? 1000F : 100000F;
            if (!float.IsFinite(value) || Math.Abs(value) > limit || offset == 108 && value < 0F)
                throw new ProtocolException("Invalid native throwable transform/lifetime.");
        }
        var path = data[112..];
        var end = path.IndexOf((byte)0);
        if (end < 1 || path[(end + 1)..].IndexOfAnyExcept((byte)0) >= 0)
            throw new ProtocolException("Invalid throwable archetype path.");
        for (var index = 0; index < end; ++index)
            if (path[index] is < 32 or > 126)
                throw new ProtocolException("Invalid throwable archetype character.");
    }

    public static byte[] Configuration(Guid session, int nativeGeneration, int worldGeneration,
        ulong localPeer, bool enabled)
    {
        var bytes = new byte[ConfigurationBytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), enabled ? 1U : 0U);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), checked((uint)Math.Max(0, nativeGeneration)));
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), checked((uint)Math.Max(0, worldGeneration)));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), session == Guid.Empty ? 0 : AISyncCodec.SessionCookie(session));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), localPeer);
        return bytes;
    }
}
