using System.Buffers.Binary;

namespace AIMP.Protocol;

// A local DLL construction/binding ticket, not a network participant identity.
// Its consumer must retain the authenticated IPC connection and native generation.
public sealed record NativeCharacterIncarnationState(
    ulong NativeGeneration, ulong Serial, uint Character, uint RuntimeId, uint Origin = 0);

public static class NativeCharacterIncarnationCodec
{
    public const int Size = 40;
    private const uint Magic = 0x464C4941;

    public static byte[] Encode(NativeCharacterIncarnationState value)
    {
        Validate(value);
        var bytes = new byte[Size];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), value.NativeGeneration);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), value.Serial);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(24), value.Character);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(28), value.RuntimeId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(32), value.Origin);
        return bytes;
    }

    public static NativeCharacterIncarnationState Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != Size || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != Magic
            || BinaryPrimitives.ReadUInt32LittleEndian(bytes[4..]) != 1
            || BinaryPrimitives.ReadUInt32LittleEndian(bytes[36..]) != 0)
            throw new ProtocolException("Invalid native character incarnation frame.");
        var value = new NativeCharacterIncarnationState(BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[16..]), BinaryPrimitives.ReadUInt32LittleEndian(bytes[24..]),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[28..]), BinaryPrimitives.ReadUInt32LittleEndian(bytes[32..]));
        Validate(value);
        return value;
    }

    private static void Validate(NativeCharacterIncarnationState value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var empty = value.NativeGeneration == 0 && value.Serial == 0 && value.Character == 0 && value.RuntimeId == 0 && value.Origin == 0;
        if (!empty && (value.NativeGeneration == 0 || value.Serial == 0 || value.Character is < 0x10000 or > uint.MaxValue - 0x50
            || value.RuntimeId == 0 || value.Origin > 1)) throw new ProtocolException("Invalid local character incarnation ticket.");
    }
}
