using System.Security.Cryptography;
using AIMP.Launcher;
using CATHODE;

namespace AIMP.Protocol.Tests;

internal static class RemoteCrouchAnimationTests
{
    internal static Task PreparationReceiptAsync()
    {
        var animation = Path.GetFullPath(Path.Combine("test-game", "DATA", "GLOBAL", "ANIMATION.PAK"));
        var level = Path.GetFullPath(Path.Combine("test-game", "DATA", "ENV", "PRODUCTION", "map", "WORLD", "COMMANDS.PAK"));
        var original = new Dictionary<string, GamePreparationService.FileStamp>(StringComparer.OrdinalIgnoreCase)
        {
            [animation] = new(1000, 100), [level] = new(2000, 200),
        };
        var current = new Dictionary<string, GamePreparationService.FileStamp>(original, StringComparer.OrdinalIgnoreCase)
        { [animation] = new(1001, 101) };
        AssertEx.True(GamePreparationService.StampsEqual(original, current, animation),
            "A separately SHA-256-verified global animation update must not rebuild unchanged levels.");
        current[level] = new(2000, 201);
        AssertEx.False(GamePreparationService.StampsEqual(original, current, animation), "A changed level still requires preparation.");
        current[level] = original[level]; current.Remove(animation);
        AssertEx.False(GamePreparationService.StampsEqual(original, current, animation), "The verified global archive must still exist.");
        current[animation] = original[animation];
        current[Path.GetFullPath("unexpected-prepared-file")] = new(1, 1);
        AssertEx.False(GamePreparationService.StampsEqual(original, current, animation), "Unexpected prepared files invalidate the receipt.");
        return Task.CompletedTask;
    }

    internal static Task RetailAsync()
    {
        var path = Environment.GetEnvironmentVariable("AIMP_TEST_ANIMATION_PAK");
        if (string.IsNullOrWhiteSpace(path))
        {
            Console.WriteLine("SKIP retail crouch animation patch: AIMP_TEST_ANIMATION_PAK is not set.");
            return Task.CompletedTask;
        }
        var pak = new PAK2(path);
        const string dbPath = @"DATA\ANIM_SYS\569978346_ANIM_CLIP_DB.BIN";
        var changed = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            dbPath, @"DATA\ANIM_SYS\ANIM_CLIP_DB.BIN", @"DATA\ANIM_SYS\SKELE\DEFS\569978346.BIN",
        };
        var before = pak.Entries.ToDictionary(e => e.Filename, e => SHA256.HashData(e.Content));
        var original = AnimClipDatabase.Parse(pak.Entries.Single(e => e.Filename == dbPath).Content);
        var originalGlobalBytes = pak.Entries.Single(e => e.Filename == @"DATA\ANIM_SYS\ANIM_CLIP_DB.BIN").Content;
        var originalGlobal = GlobalClipDatabase.Parse(originalGlobalBytes);
        AssertEx.True(originalGlobalBytes.AsSpan().SequenceEqual(originalGlobal.Serialize()),
            "Retail blend parser must round-trip all 50 blend definitions and the final table exactly.");
        RemoteCrouchAnimationInstaller.Patch(pak);
        var patched = AnimClipDatabase.Parse(pak.Entries.Single(e => e.Filename == dbPath).Content);
        var crouched = patched.Context("CROUCHED")!;
        var idle = original.Animations.Get(AnimClipDatabase.HashName("idle_crouch"))!.Value;
        var aimed = original.Context("WEAPON_SHOTGUN")!.Animations.Get(AnimClipDatabase.HashName("cover_low_aim"))!.Value;
        AssertEx.True(aimed != idle, "The aiming base must not be the unarmed crouch idle.");
        foreach (var alias in new[] { "idle_aimed", "idle_aimed_v2", "idle_to_aim", "walk_aim_on_spot_0",
            "search_aimed_idle_01", "search_aimed_idle_02", "turn_45_l_aimed", "turn_45_r_aimed",
            "turn_90_l_aimed", "turn_90_r_aimed", "turn_180_l_aimed", "turn_180_r_aimed" })
            AssertEx.Equal(aimed, crouched.Animations.Get(AnimClipDatabase.HashName(alias))!.Value,
                "Crouched weapon aiming must use its armed full-body base: " + alias);
        var global = GlobalClipDatabase.Parse(pak.Entries.Single(e => e.Filename == @"DATA\ANIM_SYS\ANIM_CLIP_DB.BIN").Content);
        foreach (var (alias, source) in new[] { ("locomotion_cycle_blend", @"HUMAN\LOCOMOTIONCYCLE"),
            ("aimed_locomotion_cycle_blend", @"HUMAN\AIMED_LOCOMOTION_CYCLE_BLEND") })
        {
            var key = AnimClipDatabase.HashName(alias);
            var target = crouched.BlendSets.Get(key)!.Value;
            var blend = global.Blend(target);
            var retail = originalGlobal.Blend(AnimClipDatabase.HashName(source));
            AssertEx.Equal(AnimClipDatabase.HashName("E_RIPLEY"), BitConverter.ToUInt32(blend, 12));
            AssertEx.Equal(AnimClipDatabase.HashName("CROUCHED"), BitConverter.ToUInt32(blend, 16));
            AssertEx.True(retail.AsSpan(0, 12).SequenceEqual(blend.AsSpan(0, 12)));
            VerifyLocomotion(pak, originalGlobal, patched, retail, blend);
            AssertEx.True(retail.AsSpan().SequenceEqual(global.Blend(AnimClipDatabase.HashName(source))),
                "The HUMAN blend used by other actors must remain byte-identical.");
            AssertEx.Equal(original.BlendSets.Get(key)!.Value, patched.BlendSets.Get(key)!.Value);
            for (var i = 0; i < blend[21]; ++i)
            {
                var sample = BitConverter.ToUInt32(blend, 65 + i * 16 + 8);
                AssertEx.True(crouched.Animations.Contains(sample), "Missing crouched locomotion sample: " + sample);
            }
        }
        // These are the actual aliases still returning standing resources in
        // v397, rather than just the projected idle alias in its snapshot.
        foreach (var name in new[] { "npc_neutral_idle_base", "npc_neutral_idle_wshift_01",
            "npc_neutral_idle_wshift_02", "npc_scared_base_idle", "npc_angry_idle_base",
            "npc_suspicious_idle_base", "aim_to_idle" })
        {
            var key = AnimClipDatabase.HashName(name);
            AssertEx.Equal(idle, crouched.Animations.Get(key)!.Value, name);
            AssertEx.Equal(original.Animations.Get(key)!.Value, patched.Animations.Get(key)!.Value,
                "Standing must retain its original clip: " + name);
        }
        foreach (var entry in pak.Entries.Where(e => !changed.Contains(e.Filename)))
            AssertEx.True(before[entry.Filename].AsSpan().SequenceEqual(SHA256.HashData(entry.Content)),
                "Unrelated animation resource changed: " + entry.Filename);
        foreach (var context in original.Contexts.Where(c => c.NameHash != AnimClipDatabase.HashName("CROUCHED")))
        {
            var actual = patched.Contexts.Single(c => c.NameHash == context.NameHash);
            AssertEx.True(context.Animations.Keys.SequenceEqual(actual.Animations.Keys));
            AssertEx.True(context.Animations.Values.SequenceEqual(actual.Animations.Values));
        }
        var first = pak.Entries.Where(e => changed.Contains(e.Filename))
            .ToDictionary(e => e.Filename, e => SHA256.HashData(e.Content));
        RemoteCrouchAnimationInstaller.Patch(pak);
        foreach (var entry in pak.Entries.Where(e => changed.Contains(e.Filename)))
            AssertEx.True(first[entry.Filename].AsSpan().SequenceEqual(SHA256.HashData(entry.Content)),
                "Patch must be idempotent: " + entry.Filename);
        Console.WriteLine("Retail NPC idle aliases, metadata, standing/weapon contexts and unrelated resources verified.");
        return Task.CompletedTask;
    }

    private static void VerifyLocomotion(PAK2 pak, GlobalClipDatabase global, AnimClipDatabase db, byte[] retail, byte[] blend)
    {
        var l = CrouchLocomotionBlend.Describe(blend);
        AssertEx.True(retail.AsSpan(20, 5).SequenceEqual(blend.AsSpan(20, 5)), "Keep blend dimensions and sample counts.");
        AssertEx.True(retail.AsSpan(49, l.Coordinates - 49).SequenceEqual(blend.AsSpan(49, l.Coordinates - 49)),
            "Keep parameter bindings and sample aliases.");
        AssertEx.True(retail.AsSpan(l.Grid, l.Rates - l.Grid).SequenceEqual(blend.AsSpan(l.Grid, l.Rates - l.Grid)),
            "Cached grid weights must remain byte-identical under the affine velocity transform.");
        AssertEx.True(retail.AsSpan(l.Indices, l.Vertices).SequenceEqual(blend.AsSpan(l.Indices, l.Vertices)));
        var context = db.Context("CROUCHED")!;
        var player = AnimClipDatabase.Parse(pak.Entries.Single(e => e.Filename == @"DATA\ANIM_SYS\2554777613_ANIM_CLIP_DB.BIN").Content);
        var directions = new Dictionary<uint, (float X, float Z)>();
        var timings = new Dictionary<uint, RetailAnimationClipTiming>();
        foreach (var (name, x, z) in new[] { ("fp_walk_f_crouch", 0F, 1F), ("fp_walk_b_crouch", 0F, -1F), ("fp_walk_l_crouch", -1F, 0F) })
        {
            var clip = player.Animations.Get(AnimClipDatabase.HashName(name))!.Value;
            var section = pak.Entries.Single(e => e.Filename == $@"DATA\ANIM_SYS\RESIDENT\ANIM_CLIP_DB_SEC_{global.SectionGroup(clip.Path)}.BIN");
            var timing = RetailAnimationClipTiming.Read(section.Content, global.SectionClipIndex(clip.Path));
            AssertEx.True(MathF.Abs(timing.Duration - 28F / 30F) < 0.0001F && MathF.Abs(timing.Speed - 2.4F) < 0.001F,
                "Read actual Havok duration and extracted distance, not the replaced standing cycle's timing.");
            timings.Add(clip.Path, timing); directions.Add(clip.Path, (x, z));
        }
        for (var i = 0; i < l.Samples; ++i)
        {
            var alias = BitConverter.ToUInt32(blend, 65 + i * 16 + 8);
            var clip = context.Animations.Get(alias)!.Value;
            if (timings.TryGetValue(clip.Path, out var timing))
                AssertEx.Equal(timing.Duration, BitConverter.ToSingle(blend, l.Durations + i * 4), "Source cycle duration.");
        }
        foreach (var (x, z) in new[] { (0F, .5F), (0F, -.5F), (.5F, 0F), (-.5F, 0F), (0F, .75F) })
        {
            var (weights, period) = Evaluate(blend, x, z);
            var moving = 0F; var vx = 0F; var vz = 0F;
            for (var i = 0; i < l.Samples; ++i)
            {
                var clip = context.Animations.Get(BitConverter.ToUInt32(blend, 65 + i * 16 + 8))!.Value;
                if (!timings.TryGetValue(clip.Path, out var timing)) continue;
                moving += weights[i];
                var direction = directions[clip.Path];
                var speed = timing.Speed * timing.Duration / period;
                vx += weights[i] * speed * direction.X * (blend[l.Mirrors + i] == 0 ? 1 : -1);
                vz += weights[i] * speed * direction.Z;
            }
            Console.WriteLine($"Crouch blend ({x},{z}): moving={moving:F3}, period={period:F3}s, extraction=({vx:F3},{vz:F3})m/s");
            AssertEx.True(moving > .9F, "Normal crouch travel must use full walking strides, without dilution by idle.");
            AssertEx.True(float.IsFinite(period) && period > 0F, "A moving blend must keep advancing.");
            AssertEx.True(MathF.Abs(vx - x) < .2F && MathF.Abs(vz - z) < .2F,
                "Playback cadence and mirrored source direction must follow the requested travel velocity.");
        }
        var (originalWeights, originalPeriod) = Evaluate(retail, 0F, .5F);
        var originalMoving = Enumerable.Range(0, l.Samples).Where(i => timings.ContainsKey(
            context.Animations.Get(BitConverter.ToUInt32(retail, 65 + i * 16 + 8))!.Value.Path)).Sum(i => originalWeights[i]);
        Console.WriteLine($"Standing-grid baseline at 0.5m/s: moving={originalMoving:F3}, period={originalPeriod:F3}s");
        AssertEx.True(originalMoving < .9F, "Reproduce idle dilution in the old standing grid.");
        AssertEx.True(originalPeriod > 0F, "Retail reference has valid timing.");
    }

    // Independent replay of the retail cached-grid evaluator (00A488E0),
    // including byte weights and synchronized duration / vertex playback rate.
    private static (float[] Weights, float Period) Evaluate(byte[] data, float x, float z)
    {
        var l = CrouchLocomotionBlend.Describe(data);
        int nx = BitConverter.ToUInt16(data, 49), na = BitConverter.ToUInt16(data, 51);
        var nz = BitConverter.ToUInt16(data, 23) / (nx * na);
        var input = new[] { x, z, 0F };
        var counts = new[] { nx, nz, na };
        var cell = new int[3]; var fraction = new float[3];
        for (var axis = 0; axis < 3; ++axis)
        {
            var value = Math.Clamp((input[axis] - BitConverter.ToSingle(data, 25 + axis * 4)) /
                BitConverter.ToSingle(data, 37 + axis * 4), 0F, counts[axis]);
            cell[axis] = Math.Min((int)value, counts[axis] - 1);
            fraction[axis] = value - cell[axis];
        }
        var weights = new float[l.Samples]; var period = 0F;
        for (var da = 0; da < 2; ++da)
        for (var dz = 0; dz < 2; ++dz)
        for (var dx = 0; dx < 2; ++dx)
        {
            var w = (dx == 0 ? 1 - fraction[0] : fraction[0]) * (dz == 0 ? 1 - fraction[1] : fraction[1]) *
                (da == 0 ? 1 - fraction[2] : fraction[2]);
            var at = l.Grid + ((cell[2] + da) * (nz + 1) * (nx + 1) + (cell[1] + dz) * (nx + 1) + cell[0] + dx) * 16;
            for (var j = 0; j < 8; ++j)
            {
                var vertex = data[at + j];
                if (vertex == 255 || vertex >= l.Vertices) continue;
                var sample = data[l.Indices + vertex];
                var weight = w * data[at + 8 + j] / 255F;
                weights[sample] += weight;
                var rate = BitConverter.ToSingle(data, l.Rates + vertex * 4);
                if (rate > 0F) period += weight * BitConverter.ToSingle(data, l.Durations + sample * 4) / rate;
            }
        }
        var sum = weights.Sum();
        if (sum > 0F) for (var i = 0; i < weights.Length; ++i) weights[i] /= sum;
        return (weights, period);
    }
}
