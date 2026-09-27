using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class WorldPublicActionTests
{
    internal static Task LocalEffectBoundaryAsync()
    {
        // A containing World/Mission graph must not turn player positioning,
        // camera manipulation or input consumption into a World effect.
        foreach (var primitive in new[] { "Interaction", "CameraShake", "PlayerCamera", "MonitorPadInput" })
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
