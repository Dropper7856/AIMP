using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class WorldPublicActionTests
{
    internal static Task RetailDoorEventCarriersAsync()
    {
        var path = Environment.GetEnvironmentVariable("AIMP_TEST_WORLD_MANIFEST");
        if (string.IsNullOrEmpty(path))
        {
            Console.WriteLine("SKIP retail Door graph: AIMP_TEST_WORLD_MANIFEST is not configured.");
            return Task.CompletedTask;
        }
        var manifest = WorldManifestBinaryCodec.Read(path);
        var door = new WorldEntityId(0xEF88CF44365CEDD6UL, 0x50B1DA79789137E2UL);
        var expected = new[]
        {
            (new WorldEntityId(0x6640047708377930UL, 0x0D6CC4A4024B852FUL), 0x4E85BB2DU),
            (new WorldEntityId(0x4EF885117FB82045UL, 0xC793E627DDE941DEUL), 0x7D4FFD73U)
        };
        foreach (var (owner, operation) in expected)
        {
            var profile = manifest.CompositePublicInputCausalProfiles.Single(p =>
                p.OwnerCompositeInstance == owner && p.EndpointKind == 2 &&
                p.SemanticOperation == operation && p.SemanticTargetEntity == door);
            AssertEx.Equal(WorldReplicationContractKind.Event, profile.ReplicationContractKind);
            AssertEx.True(WorldReplicationRelevanceCodec.Evidence(profile.ReplicationRelevance)
                .HasFlag(WorldReplicationRelevanceEvidence.EmittedEventCarrier));
            // The cut has exactly the physical Door command, no player prompt.
            var edges = manifest.Connections.Where(c => c.SourceEntity == owner &&
                c.SourceParameter == profile.PublicInputShortGuid).ToArray();
            AssertEx.True(edges.Length > 0 && edges.All(c => c.TargetEntity == door && c.TargetParameter == operation));
        }
        return Task.CompletedTask;
    }
    internal static Task LocalEffectBoundaryAsync()
    {
        // A containing World/Mission graph must not turn player positioning,
        // camera manipulation or input consumption into a World effect.
        foreach (var primitive in new[] { "Interaction", "CameraShake", "PlayerCamera", "MonitorPadInput", "MonitorActionMap", "ButtonMashPrompt" })
        {
            AssertEx.Equal(WorldDomain.Player, WorldManifestCompiler.ClassifyRuntimeDomain(
                WorldEntityKind.Function, "World/Mission/Button", primitive, "effect"));
        }
        // A World transition remains World even when its label mentions the
        // player who caused it; local-effect filtering belongs to the endpoint.
        AssertEx.Equal(WorldDomain.World, WorldManifestCompiler.ClassifyRuntimeDomain(
            WorldEntityKind.Function, "World/Mission/Button", "LogicSwitch", "PressedByPlayer"));
        // A Composite name is not the built-in Interaction primitive.
        AssertEx.Equal(WorldDomain.World, WorldManifestCompiler.ClassifyRuntimeDomain(
            WorldEntityKind.CompositeInstance, "World/Mission/Button", "Interaction", "world_branch"));
        return Task.CompletedTask;
    }
}
