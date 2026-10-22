using AIMP.Protocol;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Launcher;

internal readonly record struct MissionSyncFrontier(WorldEntityDescriptor Source, uint Signal);

internal static class MissionSyncFrontierCompiler
{
    private static readonly HashSet<uint> CompletionPorts = new[]
    {
        "used", "key_accepted", "completed", "finished", "animation_finished", "on_entered", "on_exited"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();
    // Port names are authored control contracts, not the World-domain causal hint.
    // In particular objective triggers belong to UI and carry PotentialCausalEdge=false.
    private static readonly HashSet<uint> ControlInputs = new[]
    {
        "trigger", "start", "force_open", "force_close", "enable", "disable", "activate", "deactivate"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    internal static IReadOnlyList<MissionSyncFrontier> Compile(WorldManifest manifest)
    {
        var entities = manifest.Entities.ToDictionary(value => value.EntityId);
        var sources = MissionSources(manifest);
        var roots = manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => CompletionPorts.Contains(group.Key.SourceParameter)
                && sources.ContainsKey(group.Key.SourceEntity)
                && group.All(edge => ControlInputs.Contains(edge.TargetParameter)
                    && edge.TargetEntity.IsValid && entities.TryGetValue(edge.TargetEntity, out var target)
                    && target.Domain != WorldDomain.AIMP))
            .Select(group => new MissionSyncFrontier(sources[group.Key.SourceEntity], group.Key.SourceParameter))
            .OrderBy(value => value.Source.EntityId.High).ThenBy(value => value.Source.EntityId.Low)
            .ThenBy(value => value.Signal).ToArray();
        if (roots.Length > MissionSignalValidation.MaximumSignalsPerGeneration)
            throw new InvalidOperationException("MissionSync has too many authored mission frontiers.");
        return roots;
    }

    // Runtime-addressable story-mission composites, trigger volumes and cinematics.
    private static Dictionary<WorldEntityId, WorldEntityDescriptor> MissionSources(WorldManifest manifest)
    {
        var scripts = manifest.Composites.Where(value => !value.AimpNamespace
            && value.Name.Replace('/', '\\').StartsWith("SCRIPT_StoryMission\\", StringComparison.OrdinalIgnoreCase))
            .Select(value => value.CompositeShortGuid).ToHashSet();
        return manifest.Entities.Where(value => value.EntityId.IsValid
            && value.Domain != WorldDomain.AIMP && scripts.Contains(value.SourceCompositeShortGuid)
            && (value.Flags & WorldEntityFlags.RuntimeAddressable) != 0
            && (value.Kind == WorldEntityKind.CompositeInstance
                || value.PrimitiveName is "PlayerTriggerBox" or "PlayerTriggerSphere" or "CAGEAnimation"))
            .ToDictionary(value => value.EntityId);
    }

    internal static byte[] Encode(WorldManifest manifest, IReadOnlyList<MissionSyncFrontier> roots)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(0x31534D41U);
        writer.Write(manifest.MapContentIdHigh);
        writer.Write(manifest.MapContentIdLow);
        writer.Write((uint)roots.Count);
        foreach (var root in roots)
        {
            writer.Write(root.Source.EntityId.High);
            writer.Write(root.Source.EntityId.Low);
            writer.Write(root.Source.CathodeEntityA);
            writer.Write(root.Source.CathodeEntityB);
            writer.Write(root.Signal);
        }
        return stream.ToArray();
    }
}
