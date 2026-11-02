using AIMP.Launcher;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using System.Buffers.Binary;

namespace AIMP.Protocol.Tests;

internal static class MissionFrontierTests
{
    private static uint Port(string name) => ShortGuidUtils.Generate(name).AsUInt32;
    private static readonly WorldEntityId Source = new(1, 2), Objective = new(3, 4), Door = new(5, 6);

    private static WorldEntityDescriptor Entity(WorldEntityId id, WorldEntityKind kind, WorldDomain domain) =>
        new(id, domain, kind, WorldEntityFlags.RuntimeAddressable, 100, 200, 1, 0, 200, 300, 0, 0, "", "");

    private static WorldManifest Manifest() => new(WorldManifestConstants.FormatVersion,
        WorldManifestConstants.CompilerVersion, 10, 11, 100, 0, 0,
        new WorldManifestSourceInfo("", "", true, false, true, "", ""),
        [new WorldCompositeDescriptor(100, "SCRIPT_StoryMission/M01_Pt01", WorldDomain.World, false, true)], [],
        [Entity(Source, WorldEntityKind.CompositeInstance, WorldDomain.World),
            Entity(Objective, WorldEntityKind.Function, WorldDomain.UI),
            Entity(Door, WorldEntityKind.Proxy, WorldDomain.World)], [], [], [],
        [new WorldConnectionDescriptor(Source, Port("key_accepted"), Objective, Port("trigger"), false),
            new WorldConnectionDescriptor(Source, Port("key_accepted"), Door, Port("force_open"), true)], [], [], [], 0, 0, []);

    internal static Task SelectionAsync()
    {
        var manifest = Manifest();
        var roots = MissionSyncFrontierCompiler.Compile(manifest);
        AssertEx.Equal(1, roots.Count); // UI objective is part of the same authored fan-out.
        AssertEx.Equal(Source, roots[0].Source.EntityId);
        var typedBranch = manifest with { Connections = manifest.Connections.Append(
            new WorldConnectionDescriptor(Source, Port("key_accepted"), Door, Port("position"), true)).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(typedBranch).Count);
        var nonMission = manifest with { Composites = [manifest.Composites[0] with { Name = "UTILITY/Keypad" }] };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(nonMission).Count);
        var localOnly = manifest with { Entities = manifest.Entities.Select(value =>
            value.EntityId == Source ? value with { Flags = WorldEntityFlags.None } : value).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(localOnly).Count);
        var aimp = manifest with { Entities = manifest.Entities.Select(value =>
            value.EntityId == Door ? value with { Domain = WorldDomain.AIMP } : value).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(aimp).Count);
        return Task.CompletedTask;
    }

    internal static Task SurvivalAsync()
    {
        // ChallengeMap16 ships DLC\\ChallengeMap16\\MISSION_ChallengeMap16,
        // not SCRIPT_StoryMission. Progression is an objective output, not a
        // local generator "used" interaction or a player-trigger edge.
        var story = Manifest();
        var objective = Port("obj1Completed");
        var script = story.Composites[0] with
        {
            Name = @"DLC\ChallengeMap16\MISSION_ChallengeMap16"
        };
        var survival = story with
        {
            Composites = [script],
            Connections =
            [
                new WorldConnectionDescriptor(Source, objective, Objective, Port("trigger"), false),
                new WorldConnectionDescriptor(Source, objective, Door, Port("force_open"), true)
            ]
        };
        var roots = MissionSyncFrontierCompiler.Compile(survival, survival: true);
        AssertEx.Equal(1, roots.Count);
        AssertEx.Equal(Source, roots[0].Source.EntityId);
        AssertEx.Equal(objective, roots[0].Signal);
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(survival).Count);
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(story, survival: true).Count);

        // Raw interaction "used" must never replay its sender Character.
        var interaction = survival with
        {
            Connections =
            [
                new WorldConnectionDescriptor(Source, Port("used"), Door, Port("trigger"), true)
            ]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(interaction, survival: true).Count);

        // Reject the ENTIRE output fan-out if any branch can affect a local
        // player, an AI actor or an unproved/unknown semantic target.
        foreach (var domain in new[] { WorldDomain.Player, WorldDomain.AI, WorldDomain.Unknown })
        {
            var unsafeManifest = survival with
            {
                Entities = survival.Entities.Select(value => value.EntityId == Door
                    ? value with { Domain = domain } : value).ToArray()
            };
            AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(unsafeManifest, survival: true).Count);
        }
        var dataDependent = survival with
        {
            Connections = survival.Connections.Append(new WorldConnectionDescriptor(
                Source, objective, Door, Port("position"), true)).ToArray()
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(dataDependent, survival: true).Count);
        var noRuntime = survival with
        {
            Entities = survival.Entities.Select(value => value.EntityId == Source
                ? value with { Flags = WorldEntityFlags.None } : value).ToArray()
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(noRuntime, survival: true).Count);
        return Task.CompletedTask;
    }

    // Opt-in validation against the actual extracted DLC folder. Synthetic
    // manifests validate the contract; this check proves the retail graph
    // contains an admitted objective frontier before any local output is gated.
    internal static Task ChallengeMap16GraphAsync()
    {
        var level = Environment.GetEnvironmentVariable("AIMP_TEST_CHALLENGEMAP16");
        if (string.IsNullOrWhiteSpace(level))
        {
            Console.WriteLine("SKIP MissionSync ChallengeMap16 graph: AIMP_TEST_CHALLENGEMAP16 is not set.");
            return Task.CompletedTask;
        }
        var temporary = Path.Combine(Path.GetTempPath(), $"aimp-mission-test-{Guid.NewGuid():N}.aimpwm");
        try
        {
            var manifest = new WorldManifestCompiler().Compile(level, outputPath: temporary, useCache: false).Manifest;
            var roots = MissionSyncFrontierCompiler.Compile(manifest, survival: true);
            AssertEx.True(roots.Count != 0,
                "ChallengeMap16 has zero proven objective completion frontiers. " +
                "Do not enable capture or claim that the generator is synchronized without inspecting COMMANDS.");
            Console.WriteLine($"ChallengeMap16 MissionSync: {roots.Count} safe objective frontiers.");
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        return Task.CompletedTask;
    }

    internal static Task BinaryAsync()
    {
        var manifest = Manifest();
        var roots = MissionSyncFrontierCompiler.Compile(manifest);
        var bytes = MissionSyncFrontierCompiler.Encode(manifest, roots);
        AssertEx.Equal(52, bytes.Length);
        AssertEx.Equal(0x31534D41U, BinaryPrimitives.ReadUInt32LittleEndian(bytes));
        AssertEx.Equal(1U, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(20)));
        AssertEx.Equal(Source.High, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24)));
        AssertEx.Equal(Port("key_accepted"), BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(48)));
        var reordered = manifest with { Connections = manifest.Connections.Reverse().ToArray(),
            Entities = manifest.Entities.Reverse().ToArray() };
        AssertEx.True(bytes.SequenceEqual(MissionSyncFrontierCompiler.Encode(reordered,
            MissionSyncFrontierCompiler.Compile(reordered))));
        ModelValidation.Validate(new SessionPreparedPayload(Guid.NewGuid(), 1, 1, new string('A', 64)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new SessionPreparedPayload(Guid.NewGuid(), 1, 1, "bad")));
        return Task.CompletedTask;
    }
}
