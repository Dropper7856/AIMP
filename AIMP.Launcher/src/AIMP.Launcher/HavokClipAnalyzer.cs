using System.Buffers.Binary;
using System.Numerics;
using System.Text;

namespace AIMP.Launcher;

// AnimationSync bone masks (docs/animationsync-bone-mask-analysis.md). Reads the
// Havok 2012 32-bit packfiles stored in ANIMATION.PAK: clip sections
// ([u32 N][N skeleton hashes][u32 H][packfile][u32 M][metadata]) and skeletons
// ([u32 H][packfile]...). Only hkaSplineCompressedAnimation with 8/16-bit vectors
// and ThreeComp40/uncompressed rotations is decoded; anything else is reported as
// unsupported so the clip keeps its unmasked playback.
//
// Motion is measured on the B-spline control points: a clamped spline starts on
// its first control point and stays inside their convex hull, so control-point
// amplitudes bound the curve's and the first point is the pose at t=0.
internal static class HavokClipAnalyzer
{
    internal sealed record BoneMotion(float RotationDegrees, float Translation, float RotationFromReferenceDegrees,
        float TranslationFromReference);

    internal sealed record ClipBinding(string? Unsupported, string? Skeleton, short[] TrackToBone, BoneMotion[] Tracks);

    internal sealed record Skeleton(short[] Parents, (Vector3 T, Quaternion R)[] Reference);

    internal static Skeleton ReadSkeleton(byte[] entry)
    {
        var size = BinaryPrimitives.ReadInt32LittleEndian(entry);
        if (size <= 0 || size > entry.Length - 4) throw new InvalidDataException("Skeleton payload size is invalid.");
        var pack = new Packfile(entry.AsSpan(4, size).ToArray());
        var skeleton = pack.Single("hkaSkeleton");
        var (parentsAt, parentCount) = pack.Array(skeleton, 0x0C);
        var (_, boneCount) = pack.Array(skeleton, 0x18);
        var (poseAt, poseCount) = pack.Array(skeleton, 0x24);
        if (boneCount <= 0 || boneCount > 1024 || parentCount != boneCount || poseCount != boneCount)
            throw new InvalidDataException("Skeleton arrays are inconsistent.");
        var parents = new short[boneCount];
        var reference = new (Vector3, Quaternion)[boneCount];
        for (var i = 0; i < boneCount; i++)
        {
            parents[i] = pack.Int16(parentsAt.Offset(i * 2));
            var t = poseAt.Offset(i * 0x30);
            reference[i] = (pack.Vector(t), Quaternion.Normalize(pack.Quaternion(t.Offset(0x10))));
        }
        return new Skeleton(parents, reference);
    }

    // SKELE\MAPS\<hash>: [u32 skeletonA][u32 skeletonB][u32 H][packfile]... with two
    // hkaSkeletonMapper variants, A->B first then B->A. RVA 0x646850 reads the
    // two header hashes; 0x646050 assigns them to the skeleton resources;
    // 0x6436F0 assigns variant 0's source/target and variant 1's reverse pair.
    // The serialized skeleton pointers are null, so neither pointer values nor
    // a bone-count fit can identify direction (both directions may fit).
    // Returns the simple source->target bone pairs of the requested direction, or
    // null when the mapper uses chains or does not fit the two skeletons.
    internal static (short Source, short Target)[]? ReadMapping(byte[] entry, bool aToB, int sourceBones, int targetBones)
    {
        if (entry.Length < 12) return null;
        var size = BinaryPrimitives.ReadInt32LittleEndian(entry.AsSpan(8));
        if (size <= 0 || size > entry.Length - 12) return null;
        var pack = new Packfile(entry.AsSpan(12, size).ToArray());
        var root = pack.Single("hkRootLevelContainer");
        var (variants, variantCount) = pack.Array(root, 0);
        if (variantCount != 2) return null;
        // hkRootLevelContainer::NamedVariant is {name*, className*, object*}.
        // Use container order, exactly as findObjectByType(..., previous) does
        // in the loader, rather than virtual-fixup/object address order.
        if (pack.Pointer(variants.Offset((aToB ? 0 : 1) * 12 + 8)) is not { } mapper ||
            pack.ClassOf(mapper) != "hkaSkeletonMapper") return null;
        // hkaSkeletonMapperData: simpleMappings +0x3C, stride 0x40;
        // chainMappings +0x48, stride 0x70. Chains require a different policy.
        if (pack.Array(mapper, 0x48).Count != 0) return null;
        var (simple, count) = pack.Array(mapper, 0x3C);
        if (count <= 0 || count > targetBones) return null;
        var pairs = new (short, short)[count];
        var targets = new HashSet<short>();
        for (var i = 0; i < count; i++)
        {
            var source = pack.Int16(simple.Offset(i * 0x40));
            var target = pack.Int16(simple.Offset(i * 0x40 + 2));
            if (source < 0 || source >= sourceBones || target < 0 || target >= targetBones || !targets.Add(target))
                return null;
            pairs[i] = (source, target);
        }
        return pairs;
    }

    // resolve: binding skeleton name -> that skeleton (for reference distances), or null.
    internal static IReadOnlyList<ClipBinding> ReadSection(byte[] section, Func<string?, Skeleton?> resolve)
    {
        var skeletons = BinaryPrimitives.ReadInt32LittleEndian(section);
        if (skeletons < 0 || skeletons > 64) throw new InvalidDataException("Clip section header is invalid.");
        var at = 4 + skeletons * 4;
        var size = BinaryPrimitives.ReadInt32LittleEndian(section.AsSpan(at));
        if (size <= 0 || size > section.Length - at - 4) throw new InvalidDataException("Clip section payload size is invalid.");
        var pack = new Packfile(section.AsSpan(at + 4, size).ToArray());
        // Resident sections hold several clips; the engine indexes them in the
        // container's binding order (0x638450), not in file order.
        var container = pack.Single("hkaAnimationContainer");
        var (bindingsAt, bindingCount) = pack.Array(container, 0x20);
        var result = new List<ClipBinding>();
        for (var b = 0; b < bindingCount; b++)
        {
            if (pack.Pointer(bindingsAt.Offset(b * 4)) is not { } binding || pack.ClassOf(binding) != "hkaAnimationBinding")
                throw new InvalidDataException("Animation container binding is invalid.");
            var skeleton = pack.String(binding.Offset(0x08));
            var (tracksAt, trackCount) = pack.Array(binding, 0x10);
            var map = new short[trackCount];
            for (var i = 0; i < trackCount; i++) map[i] = pack.Int16(tracksAt.Offset(i * 2));
            if (pack.Pointer(binding.Offset(0x0C)) is not { } animation)
            {
                result.Add(new ClipBinding("binding without animation", skeleton, map, []));
                continue;
            }
            var type = pack.ClassOf(animation);
            if (type != "hkaSplineCompressedAnimation")
            {
                result.Add(new ClipBinding(type ?? "unknown animation", skeleton, map, []));
                continue;
            }
            try { result.Add(new ClipBinding(null, skeleton, map, Spline(pack, animation, map, resolve(skeleton)))); }
            catch (NotSupportedException unsupported) { result.Add(new ClipBinding(unsupported.Message, skeleton, map, [])); }
        }
        return result;
    }

    // Offsets: docs/animationsync-bone-mask-analysis.md §3.5.
    private static BoneMotion[] Spline(Packfile pack, Packfile.At animation, short[] map, Skeleton? source)
    {
        var tracks = pack.Int32(animation.Offset(0x10));
        var blocks = pack.Int32(animation.Offset(0x2C));
        var (blockOffsetsAt, blockCount) = pack.Array(animation, 0x44);
        var maskSize = pack.Int32(animation.Offset(0x34));
        // Empty transformOffsets: the transform stream follows the block's masks.
        var (transformOffsetsAt, transformOffsetCount) = pack.Array(animation, 0x5C);
        var (dataAt, dataSize) = pack.Array(animation, 0x74);
        if (tracks <= 0 || tracks != map.Length || blocks <= 0 || blockCount != blocks ||
            (transformOffsetCount != 0 && transformOffsetCount != blocks) || maskSize < tracks * 4)
            throw new InvalidDataException("Spline animation arrays are inconsistent.");
        var data = pack.Bytes(dataAt, dataSize);
        var translations = new List<Vector3>[tracks];
        var rotations = new List<Quaternion>[tracks];
        for (var t = 0; t < tracks; t++) { translations[t] = []; rotations[t] = []; }
        for (var b = 0; b < blocks; b++)
        {
            var start = pack.Int32(blockOffsetsAt.Offset(b * 4));
            var reader = new Reader(data, start + (transformOffsetCount == 0 ? maskSize : pack.Int32(transformOffsetsAt.Offset(b * 4))));
            for (var t = 0; t < tracks; t++)
            {
                var mask = data.AsSpan(start + t * 4, 4);
                ReadVector(ref reader, mask[0] & 3, mask[1], Vector3.Zero, translations[t]);
                reader.Align(4);
                ReadRotation(ref reader, (mask[0] >> 2) & 15, mask[2], rotations[t]);
                reader.Align(4);
                ReadVector(ref reader, mask[0] >> 6, mask[3], Vector3.One, null);
                reader.Align(4);
            }
            var end = b + 1 < blocks ? pack.Int32(blockOffsetsAt.Offset((b + 1) * 4)) : data.Length;
            if (reader.Position > end) throw new InvalidDataException("Spline block overruns its successor.");
        }
        var result = new BoneMotion[tracks];
        for (var t = 0; t < tracks; t++)
        {
            var positions = translations[t];
            var quaternions = rotations[t];
            var min = positions[0];
            var max = positions[0];
            foreach (var p in positions) { min = Vector3.Min(min, p); max = Vector3.Max(max, p); }
            var rotation = 0F;
            for (var i = 0; i < quaternions.Count; i++)
                for (var j = i + 1; j < quaternions.Count; j++) rotation = MathF.Max(rotation, Angle(quaternions[i], quaternions[j]));
            float fromRotation = 0F, fromTranslation = 0F;
            if (source is not null && map[t] >= 0 && map[t] < source.Reference.Length)
            {
                var (rt, rr) = source.Reference[map[t]];
                fromRotation = Angle(quaternions[0], rr);
                fromTranslation = Vector3.Distance(positions[0], rt);
            }
            result[t] = new BoneMotion(rotation, Vector3.Distance(min, max), fromRotation, fromTranslation);
        }
        return result;
    }

    private static float Angle(Quaternion a, Quaternion b) =>
        2F * MathF.Acos(Math.Clamp(MathF.Abs(Quaternion.Dot(a, b)), 0F, 1F)) * (180F / MathF.PI);

    private static void ReadVector(ref Reader reader, int quantization, byte types, Vector3 identity, List<Vector3>? output)
    {
        var value = identity;
        if ((types & 0x70) == 0)
        {
            for (var axis = 0; axis < 3; axis++)
                if ((types & (1 << axis)) != 0) Set(ref value, axis, reader.Single());
            output?.Add(value);
            return;
        }
        if (quantization > 1) throw new NotSupportedException($"vector quantization {quantization}");
        var items = reader.UInt16();
        var degree = reader.Byte();
        reader.Skip(items + degree + 2);
        reader.Align(4);
        Span<float> low = stackalloc float[3];
        Span<float> high = stackalloc float[3];
        for (var axis = 0; axis < 3; axis++)
        {
            if ((types & (0x10 << axis)) != 0) { low[axis] = reader.Single(); high[axis] = reader.Single(); }
            else if ((types & (1 << axis)) != 0) Set(ref value, axis, reader.Single());
        }
        for (var i = 0; i <= items; i++)
        {
            var point = value;
            for (var axis = 0; axis < 3; axis++)
            {
                if ((types & (0x10 << axis)) == 0) continue;
                var fraction = quantization == 0 ? reader.Byte() / 255F : reader.UInt16() / 65535F;
                Set(ref point, axis, low[axis] + (high[axis] - low[axis]) * fraction);
            }
            output?.Add(point);
        }
    }

    private static void ReadRotation(ref Reader reader, int quantization, byte types, List<Quaternion> output)
    {
        var alignment = quantization switch
        {
            1 => 1, // ThreeComp40, 5 bytes
            5 => 4, // uncompressed, 16 bytes
            _ => throw new NotSupportedException($"rotation quantization {quantization}"),
        };
        if ((types & 0xF0) != 0)
        {
            var items = reader.UInt16();
            var degree = reader.Byte();
            reader.Skip(items + degree + 2);
            reader.Align(alignment);
            for (var i = 0; i <= items; i++) output.Add(ReadQuaternion(ref reader, quantization));

        }
        else if ((types & 0x0F) != 0)
        {
            reader.Align(alignment);
            output.Add(ReadQuaternion(ref reader, quantization));
        }
        else output.Add(Quaternion.Identity);
    }

    private static Quaternion ReadQuaternion(ref Reader reader, int quantization)
    {
        if (quantization == 5)
            return Quaternion.Normalize(new Quaternion(reader.Single(), reader.Single(), reader.Single(), reader.Single()));
        ulong bits = 0;
        for (var i = 0; i < 5; i++) bits |= (ulong)reader.Byte() << (i * 8);
        const ulong mask = (1UL << 12) - 1;
        const float fractal = 0.000345436F;
        Span<float> c = stackalloc float[3];
        for (var i = 0; i < 3; i++) c[i] = ((long)((bits >> (i * 12)) & mask) - (long)(mask >> 1)) * fractal;
        var w = MathF.Sqrt(MathF.Max(0F, 1F - c[0] * c[0] - c[1] * c[1] - c[2] * c[2]));
        if (((bits >> 38) & 1) != 0) w = -w;
        Span<float> q = stackalloc float[4];
        var shift = (int)((bits >> 36) & 3);
        for (int i = 0, k = 0; i < 4; i++) q[i] = i == shift ? w : c[k++];
        return Quaternion.Normalize(new Quaternion(q[0], q[1], q[2], q[3]));
    }

    private static void Set(ref Vector3 value, int axis, float component)
    {
        if (axis == 0) value.X = component; else if (axis == 1) value.Y = component; else value.Z = component;
    }

    private ref struct Reader(byte[] data, int position)
    {
        private readonly byte[] _data = data;
        internal int Position { get; private set; } = position;
        internal byte Byte() => _data[Position++];
        internal ushort UInt16() { var v = BinaryPrimitives.ReadUInt16LittleEndian(_data.AsSpan(Position)); Position += 2; return v; }
        internal float Single() { var v = BinaryPrimitives.ReadSingleLittleEndian(_data.AsSpan(Position)); Position += 4; return v; }
        internal void Skip(int count) { if (count < 0 || Position + count > _data.Length) throw new InvalidDataException("Spline data overrun."); Position += count; }
        internal void Align(int alignment) => Position = (Position + alignment - 1) / alignment * alignment;
    }

    // Havok 2012 binary packfile, 32-bit pointers, little endian. Pointers are
    // resolved through the local/global fixups, never from the raw field values.
    internal sealed class Packfile
    {
        internal readonly record struct At(int Section, int Position)
        {
            internal At Offset(int bytes) => this with { Position = Position + bytes };
        }

        private readonly byte[] _data;
        private readonly int[] _start;
        private readonly Dictionary<At, At> _pointers = [];
        private readonly Dictionary<At, string> _classes = [];

        internal Packfile(byte[] data)
        {
            _data = data;
            if (data.Length < 0x40 || BinaryPrimitives.ReadUInt32LittleEndian(data) != 0x57E0E057U ||
                BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(4)) != 0x10C0C010U || data[0x10] != 4 || data[0x11] != 1)
                throw new InvalidDataException("Not a 32-bit little-endian Havok packfile.");
            var count = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(0x14));
            if (count <= 0 || count > 16 || 0x40 + count * 0x30 > data.Length) throw new InvalidDataException("Havok section table is invalid.");
            _start = new int[count];
            var header = new int[count, 7];
            for (var s = 0; s < count; s++)
                for (var f = 0; f < 7; f++) header[s, f] = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(0x40 + s * 0x30 + 0x14 + f * 4));
            for (var s = 0; s < count; s++) _start[s] = header[s, 0];
            for (var s = 0; s < count; s++)
            {
                var b = header[s, 0];
                for (var p = b + header[s, 1]; p + 8 <= b + header[s, 2]; p += 8)
                {
                    var src = Int(p);
                    if (src == -1) continue;
                    _pointers[new At(s, src)] = new At(s, Int(p + 4));
                }
                for (var p = b + header[s, 2]; p + 12 <= b + header[s, 3]; p += 12)
                {
                    var src = Int(p);
                    if (src == -1) continue;
                    _pointers[new At(s, src)] = new At(Int(p + 4), Int(p + 8));
                }
                for (var p = b + header[s, 3]; p + 12 <= b + header[s, 4]; p += 12)
                {
                    var src = Int(p);
                    if (src == -1) continue;
                    _classes[new At(s, src)] = CString(Int(p + 4), Int(p + 8));
                }
            }
        }

        private int Int(int absolute) => BinaryPrimitives.ReadInt32LittleEndian(_data.AsSpan(absolute));
        private int Absolute(At at)
        {
            if (at.Section < 0 || at.Section >= _start.Length) throw new InvalidDataException("Havok reference is outside the packfile.");
            var absolute = _start[at.Section] + at.Position;
            if (absolute < 0 || absolute > _data.Length) throw new InvalidDataException("Havok reference is outside the packfile.");
            return absolute;
        }
        private string CString(int section, int offset)
        {
            var a = Absolute(new At(section, offset));
            var end = System.Array.IndexOf(_data, (byte)0, a);
            return Encoding.ASCII.GetString(_data, a, (end < 0 ? _data.Length : end) - a);
        }

        internal IEnumerable<At> Objects(string type) => _classes.Where(c => c.Value == type).Select(c => c.Key).OrderBy(a => a.Position);
        internal At Single(string type) => Objects(type).ToArray() is [var one] ? one : throw new InvalidDataException($"Expected one {type}.");
        internal string? ClassOf(At at) => _classes.GetValueOrDefault(at);
        internal IEnumerable<(At At, string Type)> AllObjects => _classes.Select(c => (c.Key, c.Value));
        internal string? String(At field) => Pointer(field) is { } target ? CString(target.Section, target.Position) : null;
        internal At? Pointer(At field) => _pointers.TryGetValue(field, out var target) ? target : null;
        internal int Int32(At at) => Int(Absolute(at));
        internal short Int16(At at) => BinaryPrimitives.ReadInt16LittleEndian(_data.AsSpan(Absolute(at)));
        internal float Float(At at) => BinaryPrimitives.ReadSingleLittleEndian(_data.AsSpan(Absolute(at)));
        internal Vector3 Vector(At at) => new(Float(at), Float(at.Offset(4)), Float(at.Offset(8)));
        internal Quaternion Quaternion(At at) => new(Float(at), Float(at.Offset(4)), Float(at.Offset(8)), Float(at.Offset(12)));

        // hkArray<T> on 32-bit: { T* data; int size; int capacityAndFlags }.
        internal (At Data, int Count) Array(At owner, int field)
        {
            var count = Int32(owner.Offset(field + 4));
            if (count == 0) return (default, 0);
            if (count < 0 || Pointer(owner.Offset(field)) is not { } data) throw new InvalidDataException("Havok array has no data.");
            return (data, count);
        }

        internal byte[] Bytes(At at, int count)
        {
            var a = Absolute(at);
            if (count < 0 || a + count > _data.Length) throw new InvalidDataException("Havok byte array is outside the packfile.");
            return _data.AsSpan(a, count).ToArray();
        }
    }
}
