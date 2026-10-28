using System.Buffers.Binary;

namespace AIMP.Protocol;

/// <summary>An immutable, exact native call payload. Null denotes the empty extension.</summary>
public sealed class AISyncNativeIntent : IEquatable<AISyncNativeIntent>
{
    private readonly uint[] words;
    public AISyncNativeIntent(ReadOnlySpan<uint> value)
    {
        words = value.ToArray();
        AISyncNativeIntentCodec.ValidateWords(words);
    }
    public uint ContractId => words[0];
    public ReadOnlySpan<uint> Words => words;
    public bool Equals(AISyncNativeIntent? other) => other is not null && Words.SequenceEqual(other.Words);
    public override bool Equals(object? obj) => obj is AISyncNativeIntent other && Equals(other);
    public override int GetHashCode()
    {
        var hash = new HashCode();
        foreach (var word in words) hash.Add(word);
        return hash.ToHashCode();
    }
    public static bool operator ==(AISyncNativeIntent? left, AISyncNativeIntent? right)
        => ReferenceEquals(left, right) || (left is not null && left.Equals(right));
    public static bool operator !=(AISyncNativeIntent? left, AISyncNativeIntent? right) => !(left == right);
}

public static class AISyncNativeIntentCodec
{
    public const uint Kind = 0x200, Profile = 128;
    public const int Bytes = 144, WordCount = 36;
    public static void Validate(AISyncNativeIntent? value)
    {
        if (value is not null) ValidateWords(value.Words);
    }
    internal static void ValidateWords(ReadOnlySpan<uint> value)
    {
        if (value.Length != WordCount) throw new ProtocolException("Invalid native intent word count.");
        var id = value[0];
        var valid = id is >= 1 and <= 20 and not (11 or 14 or 18) && value[35] == 0;
        if (valid && id == 20)
        {
            valid = (value[1] & ~15U) == 0 && Zero(value, 2, 12) && value[20] == 0 && value[21] != 0 && value[22] != 0
                && Integer(value[23], 1000000) && Integer(value[24], 1000000) && Integer(value[25], 65535)
                && Real(value[26], 0, 64) && Real(value[27], 0, 60) && Real(value[28], 0, 60)
                && Real(value[30], -1, 1000000) && Real(value[31], 0, 60)
                && Real(value[32], -100000, 100000) && (value[33] & ~0x03FFFFFFU) == 0 && value[34] == 0;
            for (var i = 12; i < 20; i += 2)
                valid &= (value[i] != 0 || value[i + 1] == 0)
                    && ((value[1] & (1U << ((i - 12) / 2))) == 0 || value[i] != 0);
        }
        else if (valid)
        {
            valid = Zero(value, 12, 35);
            if (id <= 7) valid &= Zero(value, 1, 12);
            else if (id is 8 or 9 or 10 or 12 or 13)
                valid &= value[1] == (id is 9 or 12 ? 0x11U : 1U) && (id is 9 or 12 || value[3] == 0)
                    && Zero(value, 4, 12) && (id != 13 || float.IsFinite(BitConverter.UInt32BitsToSingle(value[2])));
            else if (id is >= 15 and <= 17)
                valid &= Zero(value, 2, 4) && value[5] == 0 && Zero(value, 7, 12)
                    && ((id == 16 && value[1] == 5 && value[4] == 0 && value[6] == 0)
                        || (value[1] == 2 && (value[4] != 0 || value[6] != 0)));
            else
            {
                valid &= value[1] == 4 && Zero(value, 2, 8);
                for (var i = 8; i < 12; ++i) valid &= float.IsFinite(BitConverter.UInt32BitsToSingle(value[i]));
            }
        }
        if (!valid) throw new ProtocolException("Unsupported or noncanonical native intent payload.");
    }
    private static bool Zero(ReadOnlySpan<uint> value, int begin, int end)
    {
        for (var i = begin; i < end; ++i) if (value[i] != 0) return false;
        return true;
    }
    private static bool Integer(uint value, int high) => unchecked((int)value) >= -1 && unchecked((int)value) <= high;
    private static bool Real(uint value, float low, float high)
    {
        var f = BitConverter.UInt32BitsToSingle(value);
        return float.IsFinite(f) && f >= low && f <= high;
    }
    public static byte[] Encode(AISyncNativeIntent? value)
    {
        var bytes = new byte[Bytes];
        Encode(value, bytes);
        return bytes;
    }
    public static void Encode(AISyncNativeIntent? value, Span<byte> bytes)
    {
        if (bytes.Length != Bytes) throw new ProtocolException("Invalid native intent byte count.");
        Validate(value);
        if (value is null) { bytes.Clear(); return; }
        for (var i = 0; i < WordCount; ++i)
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.Slice(i * 4, 4), value.Words[i]);
    }
    public static AISyncNativeIntent? Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != Bytes) throw new ProtocolException("Invalid native intent byte count.");
        Span<uint> words = stackalloc uint[WordCount];
        var empty = true;
        for (var i = 0; i < WordCount; ++i)
        {
            words[i] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(i * 4, 4));
            empty &= words[i] == 0;
        }
        return empty ? null : new AISyncNativeIntent(words);
    }
}
