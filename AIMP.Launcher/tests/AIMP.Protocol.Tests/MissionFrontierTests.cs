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
