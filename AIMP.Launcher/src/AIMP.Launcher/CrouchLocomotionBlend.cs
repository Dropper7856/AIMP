using System.Text;

namespace AIMP.Launcher;

/// <summary>Read timing from the shipped x86 Havok binding, without sampling or rewriting its compressed tracks.</summary>
internal readonly record struct RetailAnimationClipTiming(float Duration, float Speed)
{
    public static RetailAnimationClipTiming Read(byte[] section, uint clipIndex)
    {
        static uint U(byte[] bytes, int at)
        {
            if (at < 0 || at > bytes.Length - 4) throw new InvalidDataException("Truncated animation data.");
            return BitConverter.ToUInt32(bytes, at);
        }
        var names = checked((int)U(section, 0));
        var sizeAt = checked(4 + names * 4);
        var size = checked((int)U(section, sizeAt));
        if (size < 64 || sizeAt + 4L + size > section.Length) throw new InvalidDataException("Invalid Havok section size.");
        var data = section.AsSpan(sizeAt + 4, size).ToArray();
        if (U(data, 0) != 0x57E0E057U || U(data, 4) != 0x10C0C010U || U(data, 12) != 9U ||
            U(data, 16) != 0x01000104U || Encoding.ASCII.GetString(data, 40, 14) != "hk_2012.2.0-r1")
            throw new InvalidDataException("Unsupported animation Havok layout.");
        var count = checked((int)U(data, 20));
        if (count is < 1 or > 16 || 64L + count * 48 > data.Length) throw new InvalidDataException("Invalid Havok sections.");
        var starts = new int[count];
        for (var i = 0; i < count; ++i) starts[i] = checked((int)U(data, 64 + i * 48 + 20));
        var pointers = new Dictionary<int, int>();
        for (var i = 0; i < count; ++i)
        {
            var header = 64 + i * 48;
            var start = starts[i];
            var local = checked((int)U(data, header + 24));
            var global = checked((int)U(data, header + 28));
            var virt = checked((int)U(data, header + 32));
            var end = checked((int)U(data, header + 44));
            if (start < 0 || local < 0 || global < local || virt < global || end < virt || start + (long)end > data.Length)
                throw new InvalidDataException("Invalid Havok relocation ranges.");
            for (var at = checked(start + local); at + 8 <= start + global; at += 8)
            {
                var from = U(data, at);
                if (from == uint.MaxValue) continue;
                pointers.Add(checked(start + (int)from), checked(start + (int)U(data, at + 4)));
            }
            for (var at = checked(start + global); at + 12 <= start + virt; at += 12)
            {
                var from = U(data, at);
                if (from == uint.MaxValue) continue;
                var target = U(data, at + 4);
                if (target >= count) throw new InvalidDataException("Invalid Havok relocation target.");
                pointers.Add(checked(start + (int)from), checked(starts[target] + (int)U(data, at + 8)));
            }
        }
        int Pointer(int at) => pointers.TryGetValue(at, out var p) && p >= 0 && p < data.Length
            ? p : throw new InvalidDataException("Missing Havok animation pointer.");
        var rootSection = U(data, 24);
        var classSection = U(data, 32);
        if (rootSection >= count || classSection >= count) throw new InvalidDataException("Invalid Havok root.");
        var classAt = checked(starts[classSection] + (int)U(data, 36));
        if (classAt < 0 || classAt + 22 > data.Length || Encoding.ASCII.GetString(data, classAt, 22) != "hkaAnimationContainer\0")
            throw new InvalidDataException("The Havok root is not an animation container.");
        var root = checked(starts[rootSection] + (int)U(data, 28));
        if (clipIndex >= U(data, root + 36)) throw new InvalidDataException("Animation binding index is out of range.");
        var binding = Pointer(checked(Pointer(root + 32) + (int)clipIndex * 4));
        var animation = Pointer(binding + 12);
        var duration = BitConverter.Int32BitsToSingle(unchecked((int)U(data, animation + 12)));
        var reference = Pointer(animation + 24);
        var samples = U(data, reference + 56);
        if (samples is < 2 or > 100000) throw new InvalidDataException("Invalid animation extraction samples.");
        var first = Pointer(reference + 52);
        var last = checked(first + ((int)samples - 1) * 16);
        float F(int at) => BitConverter.Int32BitsToSingle(unchecked((int)U(data, at)));
        var dx = F(last) - F(first);
        var dz = F(last + 8) - F(first + 8);
        var speed = MathF.Sqrt(dx * dx + dz * dz) / duration;
        if (!float.IsFinite(duration) || duration is < 0.05F or > 10F || !float.IsFinite(speed) || speed is < 0.1F or > 10F)
            throw new InvalidDataException("Invalid crouch animation timing or extraction speed.");
        return new(duration, speed);
    }
}

/// <summary>Adapt a version-4 retail 3D blend while retaining its cached interpolation topology.</summary>
internal static class CrouchLocomotionBlend
{
    internal const float VelocityScale = 0.25F;

    internal readonly record struct Layout(int Samples, int Vertices, int Coordinates, int Grid, int Rates, int Indices, int Durations, int Mirrors);

    internal static Layout Describe(byte[] blend)
    {
        if (blend.Length < 65 || BitConverter.ToUInt64(blend, 0) != 4 || blend[20] != 3)
            throw new InvalidDataException("Expected a retail 3D locomotion blend.");
        int samples = blend[21], vertices = blend[22];
        var cells = BitConverter.ToUInt16(blend, 23);
        var x = BitConverter.ToUInt16(blend, 49);
        var y = BitConverter.ToUInt16(blend, 51);
        if (x == 0 || y == 0 || cells % (x * y) != 0) throw new InvalidDataException("Invalid locomotion grid.");
        var coordinates = checked(65 + samples * 16);
        var grid = checked(coordinates + vertices * 12);
        var rates = checked(grid + (x + 1) * (y + 1) * (cells / (x * y) + 1) * 16);
        var indices = checked(rates + vertices * 4);
        var durations = checked(indices + vertices);
        var mirrors = checked(durations + samples * 4);
        if (samples == 0 || vertices == 0 || mirrors + samples != blend.Length)
            throw new InvalidDataException("Truncated locomotion blend.");
        return new(samples, vertices, coordinates, grid, rates, indices, durations, mirrors);
    }

    internal static void Adapt(byte[] blend, IReadOnlyDictionary<uint, RetailAnimationClipTiming> timing, IReadOnlySet<uint> rightward)
    {
        var l = Describe(blend);
        float F(int at) => BitConverter.ToSingle(blend, at);
        void Put(int at, float value) => BitConverter.GetBytes(value).CopyTo(blend, at);
        // Minima + grid spacing for X/Z linear velocity. Angular velocity and
        // every cached interpolation weight stay unchanged (00A47B20/A47D40).
        foreach (var at in new[] { 25, 29, 37, 41 }) Put(at, F(at) * VelocityScale);
        for (var i = 0; i < l.Samples; ++i)
        {
            var alias = BitConverter.ToUInt32(blend, 65 + i * 16 + 8);
            if (timing.TryGetValue(alias, out var clip)) Put(l.Durations + i * 4, clip.Duration);
            if (rightward.Contains(alias)) blend[l.Mirrors + i] ^= 1;
        }
        for (var i = 0; i < l.Vertices; ++i)
        {
            var at = l.Coordinates + i * 12;
            var x = F(at) * VelocityScale;
            var z = F(at + 4) * VelocityScale;
            Put(at, x); Put(at + 4, z);
            var sample = blend[l.Indices + i];
            if (sample >= l.Samples) throw new InvalidDataException("Invalid locomotion vertex sample.");
            var alias = BitConverter.ToUInt32(blend, 65 + sample * 16 + 8);
            if (!timing.TryGetValue(alias, out var clip)) continue;
            // 00A488E0 computes synchronized period = sum(weight * duration /
            // vertex rate), then per-sample playback speed = duration / period.
            // Rate is distance/time in this vertex divided by authored speed.
            var rate = MathF.Sqrt(x * x + z * z) / clip.Speed;
            if (!float.IsFinite(rate) || rate <= 0F) throw new InvalidDataException("Moving crouch sample has zero velocity.");
            Put(l.Rates + i * 4, rate);
        }
    }
}
