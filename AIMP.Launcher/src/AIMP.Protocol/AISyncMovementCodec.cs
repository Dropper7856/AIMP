using System.Buffers.Binary;

namespace AIMP.Protocol;

public enum AISyncMovementNavigation : uint { LocalNative = 1, HostRoute = 2 }
public readonly record struct AISyncMovementParameters(uint Retained, uint Style,
    float X, float Y, float Z, float StoppingDistance, float StopDistance,
    float Query0, float Query1, float Query2, float Query3, float Query4,
    float Query5, float Query6, float Query7, float Query8, uint Flags90, uint Flags91);
public readonly record struct AISyncMovementBundle(uint Version, uint Parts, uint Gait,
    AISyncMovementNavigation Navigation, AISyncMovementParameters Before,
    AISyncMovementParameters After, AISyncMovementParameters Companion,
    uint CompanionFlags = 0, uint CompanionRetainedAfter = 0, uint PrimaryFlags = 0);

// Float outputs +80..A0 include the virtual+410 results at +94/+98.
// No native reference, pointer, callback/list or execution cursor crosses here.
public static class AISyncMovementCodec
{
    public const int Bytes = 244, ParametersBytes = 72;
    public static bool IsEmpty(AISyncMovementBundle value)
    {
        Span<byte> bytes = stackalloc byte[Bytes]; Write(value, bytes);
        foreach (var b in bytes) if (b != 0) return false;
        return true;
    }
    private static bool IsEmpty(AISyncMovementParameters value)
    {
        Span<byte> bytes = stackalloc byte[ParametersBytes]; WriteParameters(value, bytes);
        foreach (var b in bytes) if (b != 0) return false;
        return true;
    }
    public static void Validate(AISyncMovementBundle value)
    {
        if (IsEmpty(value)) return;
        if (value.Version != 1 || value.Parts is < 1 or > 2 || value.Gait > 3 || value.PrimaryFlags > 7
            || value.Navigation is not (AISyncMovementNavigation.LocalNative or AISyncMovementNavigation.HostRoute))
            throw new ProtocolException("Invalid AISync movement bundle profile.");
        ValidateParameters(value.Before); ValidateParameters(value.After);
        if (value.Parts == 2)
        {
            ValidateParameters(value.Companion);
            if (value.CompanionFlags > 7 || value.CompanionRetainedAfter > 1)
                throw new ProtocolException("Invalid companion retention or behavior flags.");
        }
        else if (!IsEmpty(value.Companion) || value.CompanionFlags != 0 || value.CompanionRetainedAfter != 0)
            throw new ProtocolException("Absent movement companion has nonempty parameters.");
    }
    private static void ValidateParameters(AISyncMovementParameters value)
    {
        if (value.Retained > 1 || value.Style > 4 || (value.Flags90 & ~0x3FU) != 0 || (value.Flags91 & ~0x31U) != 0
            || value.StoppingDistance < -1 || value.StopDistance < -1)
            throw new ProtocolException("Invalid native movement choices.");
        // Query words are bit-exact native copies (possibly uninitialized), never validated.
        foreach (var f in new[] { value.X, value.Y, value.Z, value.StoppingDistance, value.StopDistance })
            if (!float.IsFinite(f) || MathF.Abs(f) > 100000F) throw new ProtocolException("Invalid movement scalar.");
    }
    public static byte[] Encode(AISyncMovementBundle value)
    { Validate(value); var bytes = new byte[Bytes]; Write(value, bytes); return bytes; }
    public static AISyncMovementBundle Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != Bytes) throw new ProtocolException("Invalid movement bundle length.");
        var value = new AISyncMovementBundle(U32(bytes, 0), U32(bytes, 4), U32(bytes, 8),
            (AISyncMovementNavigation)U32(bytes, 12), ReadParameters(bytes.Slice(16, 72)),
            ReadParameters(bytes.Slice(88, 72)), ReadParameters(bytes.Slice(160, 72)), U32(bytes, 232), U32(bytes, 236), U32(bytes, 240));
        Validate(value); return value;
    }
    private static void Write(AISyncMovementBundle v, Span<byte> bytes)
    {
        W32(bytes, 0, v.Version); W32(bytes, 4, v.Parts); W32(bytes, 8, v.Gait); W32(bytes, 12, (uint)v.Navigation);
        WriteParameters(v.Before, bytes.Slice(16, 72)); WriteParameters(v.After, bytes.Slice(88, 72));
        WriteParameters(v.Companion, bytes.Slice(160, 72));
        W32(bytes, 232, v.CompanionFlags); W32(bytes, 236, v.CompanionRetainedAfter);
        W32(bytes, 240, v.PrimaryFlags);
    }
    private static float[] Floats(AISyncMovementParameters v) => new[] { v.X, v.Y, v.Z, v.StoppingDistance,
        v.StopDistance, v.Query0, v.Query1, v.Query2, v.Query3, v.Query4, v.Query5, v.Query6, v.Query7, v.Query8 };
    private static void WriteParameters(AISyncMovementParameters v, Span<byte> bytes)
    {
        W32(bytes, 0, v.Retained); W32(bytes, 4, v.Style); var floats = Floats(v);
        for (var i = 0; i < floats.Length; i++) W32(bytes, 8 + i * 4, BitConverter.SingleToUInt32Bits(floats[i]));
        W32(bytes, 64, v.Flags90); W32(bytes, 68, v.Flags91);
    }
    private static AISyncMovementParameters ReadParameters(ReadOnlySpan<byte> b) => new(U32(b, 0), U32(b, 4),
        F(b, 8), F(b, 12), F(b, 16), F(b, 20), F(b, 24), F(b, 28), F(b, 32), F(b, 36), F(b, 40),
        F(b, 44), F(b, 48), F(b, 52), F(b, 56), F(b, 60), U32(b, 64), U32(b, 68));
    private static float F(ReadOnlySpan<byte> b, int at) => BitConverter.UInt32BitsToSingle(U32(b, at));
    private static uint U32(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b[at..]);
    private static void W32(Span<byte> b, int at, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(b[at..], value);
}
