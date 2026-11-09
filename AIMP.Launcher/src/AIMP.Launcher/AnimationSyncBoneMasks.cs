using CATHODE;

namespace AIMP.Launcher;

// AnimationSync: per-bone motion descriptors of every clip the local player can
// play (1ST_PERSON_RIPLEY clip DB), computed from the retail ANIMATION.PAK during
// preparation. The DLL looks up the clip it replays on a crew representation and
// keeps that representation's own pose on the bones the clip does not drive
// (docs/animationsync-bone-mask-analysis.md). Raw measurements are stored, not a
// decision, so the runtime policy can change without regenerating the file.
// Bone indices are in each clip's own source skeleton (FEMALEFP, MALE, ...).
//
// <game>\AIMP\AnimationSync\bone-masks.bin, little endian:
//   u32 magic 'AIBM', u32 version, byte[32] SHA-256 of the retail ANIMATION.PAK,
//   u32 skeletonCount, skeletonCount x { u32 hash, u32 boneCount,
//                                         i16 parents[boneCount], padded to 4 },
//   u32 mapCount, mapCount x { u32 source, u32 target, u32 targetBones, u32 pairCount,
//                              pairCount x { i16 sourceBone, i16 targetBone } }
//     (every SKELE/MAPS retarget table of the engine, both directions),
//   u32 clipCount, clips sorted by path hash:
//     u32 pathHash, u32 status (ClipStatus), u32 skeletonHash, u32 trackCount,
//     trackCount x { i16 bone, u16 0, f32 rotationDegrees, f32 translation,
//                    f32 rotationFromReferenceDegrees, f32 translationFromReference }
internal static class AnimationSyncBoneMasks
{
    internal const uint Magic = 0x4D424941; // "AIBM"
    internal const uint Version = 4;
    private const string PlayerClipDbEntry = @"DATA\ANIM_SYS\2554777613_ANIM_CLIP_DB.BIN"; // 1ST_PERSON_RIPLEY
    private const string GlobalClipDbEntry = @"DATA\ANIM_SYS\ANIM_CLIP_DB.BIN";

    internal enum ClipStatus : uint { Measured = 0, UnsupportedFormat = 1, MissingSkeleton = 2, MissingSection = 3, Invalid = 4 }

    internal static string PathFor(string game) => Path.Combine(game, "AIMP", "AnimationSync", "bone-masks.bin");

    // Never fails preparation: without a valid file the DLL replays clips unmasked.
    internal static void Ensure(string game, string retailPak, string retailSha256, CancellationToken token)
    {
        var path = PathFor(game);
        var error = path + ".error.txt";
        var sha = Convert.FromHexString(retailSha256);
        try
        {
            if (Current(path, sha)) return;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var bytes = Build(new PAK2(retailPak), sha, token);
            File.WriteAllBytes(path + ".tmp", bytes);
            File.Move(path + ".tmp", path, true);
            if (File.Exists(error)) File.Delete(error);
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception exception)
        {
            try
            {
                if (File.Exists(path)) File.Delete(path);
                File.WriteAllText(error, exception.ToString());
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    private static bool Current(string path, byte[] sha)
    {
        if (!File.Exists(path)) return false;
        using var reader = new BinaryReader(File.OpenRead(path));
        return reader.BaseStream.Length >= 40 && reader.ReadUInt32() == Magic && reader.ReadUInt32() == Version &&
            reader.ReadBytes(32).AsSpan().SequenceEqual(sha);
    }

    private sealed record Measurement(uint Path, ClipStatus Status, uint Skeleton, HavokClipAnalyzer.ClipBinding? Binding);

    internal static byte[] Build(PAK2 pak, byte[] retailSha, CancellationToken token)
    {
        var entries = pak.Entries.GroupBy(e => e.Filename, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(g => g.Key, g => g.First().Content, StringComparer.OrdinalIgnoreCase);
        byte[] Entry(string name) => entries.TryGetValue(name, out var content) ? content
            : throw new InvalidDataException($"ANIMATION.PAK does not contain {name}.");

        var skeletons = new Dictionary<uint, HavokClipAnalyzer.Skeleton?>();
        HavokClipAnalyzer.Skeleton? Skeleton(uint hash)
        {
            if (skeletons.TryGetValue(hash, out var known)) return known;
            HavokClipAnalyzer.Skeleton? skeleton = null;
            if (entries.TryGetValue($@"DATA\ANIM_SYS\SKELE\SK\{hash}", out var raw))
            {
                try { skeleton = HavokClipAnalyzer.ReadSkeleton(raw); }
                catch (InvalidDataException) { }
            }
            return skeletons[hash] = skeleton;
        }

        var player = AnimClipDatabase.Parse(Entry(PlayerClipDbEntry));
        var global = GlobalClipDatabase.Parse(Entry(GlobalClipDbEntry));
        var sections = new Dictionary<string, IReadOnlyList<HavokClipAnalyzer.ClipBinding>?>(StringComparer.OrdinalIgnoreCase);
        var measured = new List<Measurement>();
        foreach (var path in player.Animations.Values.Select(c => c.Path).Distinct().Order())
        {
            token.ThrowIfCancellationRequested();
            measured.Add(Measure(path, global, entries, sections, Skeleton));
        }

        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write(retailSha);
        var used = measured.Where(m => m.Status == ClipStatus.Measured).Select(m => m.Skeleton).Distinct().Order().ToArray();
        writer.Write((uint)used.Length);
        foreach (var hash in used)
        {
            var parents = skeletons[hash]!.Parents;
            writer.Write(hash);
            writer.Write((uint)parents.Length);
            foreach (var parent in parents) writer.Write(parent);
            if (parents.Length % 2 != 0) writer.Write((short)0);
        }
        var maps = Maps(entries, Skeleton);
        writer.Write((uint)maps.Count);
        foreach (var (source, target, targetBones, pairs) in maps)
        {
            writer.Write(source);
            writer.Write(target);
            writer.Write((uint)targetBones);
            writer.Write((uint)pairs.Length);
            foreach (var (from, to) in pairs) { writer.Write(from); writer.Write(to); }
        }
        writer.Write((uint)measured.Count);
        foreach (var m in measured)
        {
            writer.Write(m.Path);
            writer.Write((uint)m.Status);
            writer.Write(m.Skeleton);
            var tracks = m.Status == ClipStatus.Measured ? m.Binding!.Tracks : [];
            writer.Write((uint)tracks.Length);
            for (var t = 0; t < tracks.Length; t++)
            {
                writer.Write(m.Binding!.TrackToBone[t]);
                writer.Write((ushort)0);
                writer.Write(tracks[t].RotationDegrees);
                writer.Write(tracks[t].Translation);
                writer.Write(tracks[t].RotationFromReferenceDegrees);
                writer.Write(tracks[t].TranslationFromReference);
            }
        }
        writer.Flush();
        return stream.ToArray();
    }

    private static List<(uint Source, uint Target, int TargetBones, (short, short)[] Pairs)> Maps(
        Dictionary<string, byte[]> entries, Func<uint, HavokClipAnalyzer.Skeleton?> skeleton)
    {
        var result = new List<(uint, uint, int, (short, short)[])>();
        foreach (var (name, content) in entries.OrderBy(e => e.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (!name.StartsWith(@"DATA\ANIM_SYS\SKELE\MAPS\", StringComparison.OrdinalIgnoreCase) || content.Length < 12) continue;
            uint a = BitConverter.ToUInt32(content, 0), b = BitConverter.ToUInt32(content, 4);
            foreach (var (source, target, aToB) in new[] { (a, b, true), (b, a, false) })
            {
                if (skeleton(source) is not { } from || skeleton(target) is not { } to) continue;
                try
                {
                    if (HavokClipAnalyzer.ReadMapping(content, aToB, from.Parents.Length, to.Parents.Length) is { } pairs)
                        result.Add((source, target, to.Parents.Length, pairs));
                }
                catch (InvalidDataException) { }
                catch (ArgumentOutOfRangeException) { }
            }
        }
        return result;
    }

    private static Measurement Measure(uint path, GlobalClipDatabase global, Dictionary<string, byte[]> entries,
        Dictionary<string, IReadOnlyList<HavokClipAnalyzer.ClipBinding>?> sections,
        Func<uint, HavokClipAnalyzer.Skeleton?> skeleton)
    {
        if (global.SectionGroup(path) is not uint group) return new(path, ClipStatus.MissingSection, 0, null);
        var index = global.SectionClipIndex(path);
        var streamed = index == uint.MaxValue;
        var name = $@"DATA\ANIM_SYS\{(streamed ? "STREAMED" : "RESIDENT")}\ANIM_CLIP_DB_SEC_{group}.BIN";
        if (!sections.TryGetValue(name, out var bindings))
        {
            bindings = null;
            if (entries.TryGetValue(name, out var section))
            {
                try { bindings = HavokClipAnalyzer.ReadSection(section, n => n is null ? null : skeleton(AnimClipDatabase.HashName(n))); }
                catch (InvalidDataException) { bindings = []; }
                catch (ArgumentOutOfRangeException) { bindings = []; }
                catch (IndexOutOfRangeException) { bindings = []; }
            }
            sections[name] = bindings;
        }
        if (bindings is null) return new(path, ClipStatus.MissingSection, 0, null);
        var slot = streamed ? 0 : checked((int)index);
        if (slot >= bindings.Count) return new(path, ClipStatus.Invalid, 0, null);
        var binding = bindings[slot];
        var hash = binding.Skeleton is null ? 0U : AnimClipDatabase.HashName(binding.Skeleton);
        if (binding.Unsupported is not null) return new(path, ClipStatus.UnsupportedFormat, hash, null);
        if (hash == 0U || skeleton(hash) is not { } source) return new(path, ClipStatus.MissingSkeleton, hash, null);
        if (binding.Tracks.Length == 0 || binding.Tracks.Length != binding.TrackToBone.Length ||
            binding.TrackToBone.Any(b => b < -1 || b >= source.Parents.Length))
            return new(path, ClipStatus.Invalid, hash, null);
        return new(path, ClipStatus.Measured, hash, binding);
    }
}
