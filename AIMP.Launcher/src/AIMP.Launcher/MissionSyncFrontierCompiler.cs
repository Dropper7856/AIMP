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
    // Challenge-map mission progress uses explicit authored completion outputs.
    // These labels occur in CHALLENGEMAP16/COMMANDS.PAK, unlike the story-only
    // SCRIPT_StoryMission namespace. Never capture a raw generator "used",
    // "on_entered", animation-start or player Interaction event in Survival:
    // its callback may carry a local Character and cause remote repositioning.
    private static readonly HashSet<uint> SurvivalCompletionPorts = new[]
    {
        "obj1Completed", "obj2Completed", "obj3Completed",
        "level_complete", "MissionComplete", "LevelComplete"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();
    // Terminal objective/UI controls are scalar-free decisions. Ordinary
    // interface inputs still require the existing replay-safe control contract.
    private static readonly HashSet<uint> SurvivalControlInputs = new[]
    {
        "Objective 1 Complete", "Objective 2 Complete", "Objective 3 Complete",
        "obj1Completed", "obj2Completed", "obj3Completed",
        "MissionComplete", "LevelComplete", "level_complete",
        "gotoEndScreen", "showSplash", "setObjIcons"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // Port names are authored control contracts, not the World-domain causal hint.
    // In particular objective triggers belong to UI and carry PotentialCausalEdge=false.
    private static readonly HashSet<uint> ControlInputs = new[]
    {
        "trigger", "start", "force_open", "force_close", "enable", "disable", "activate", "deactivate"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    internal static IReadOnlyList<MissionSyncFrontier> Compile(
        WorldManifest manifest, bool survival = false)
    {
        var entities = manifest.Entities.ToDictionary(value => value.EntityId);
        var sources = MissionSources(manifest, survival);
        var completion = survival ? SurvivalCompletionPorts : CompletionPorts;
        var roots = manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => completion.Contains(group.Key.SourceParameter)
                && sources.ContainsKey(group.Key.SourceEntity)
                && group.All(edge => SafeTarget(edge, entities, survival)))
            .Select(group => new MissionSyncFrontier(sources[group.Key.SourceEntity], group.Key.SourceParameter))
            .OrderBy(value => value.Source.EntityId.High).ThenBy(value => value.Source.EntityId.Low)
            .ThenBy(value => value.Signal).ToArray();
        if (roots.Length > MissionSignalValidation.MaximumSignalsPerGeneration)
            throw new InvalidOperationException("MissionSync has too many authored mission frontiers.");
        return roots;
    }

    private static bool SafeTarget(
        WorldConnectionDescriptor edge,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities,
        bool survival)
    {
        if (!edge.TargetEntity.IsValid || !entities.TryGetValue(edge.TargetEntity, out var target)
            || target.Domain == WorldDomain.AIMP) return false;
        if (!ControlInputs.Contains(edge.TargetParameter)
            && !(survival && SurvivalControlInputs.Contains(edge.TargetParameter))) return false;
        // A Survival commit must never drive a local Player, an AI actor, or an
        // unknown CATHODE endpoint. Replaying any of those can trigger a
        // player interaction/teleport instead of merely completing an objective.
        return !survival || target.Domain is WorldDomain.World or WorldDomain.UI;
    }

    // Runtime-addressable story frontiers retain their existing contract.
    // Survival must be compiled separately: the shipped DLC uses
    // DLC\\ChallengeMap16\\MISSION_ChallengeMap16, not SCRIPT_StoryMission.
    private static Dictionary<WorldEntityId, WorldEntityDescriptor> MissionSources(
        WorldManifest manifest, bool survival)
    {
        var scripts = manifest.Composites.Where(value => !value.AimpNamespace
            && (survival ? IsSurvivalMissionScript(value.Name)
                : value.Name.Replace('/', '\\').StartsWith("SCRIPT_StoryMission\\",
                    StringComparison.OrdinalIgnoreCase)))
            .Select(value => value.CompositeShortGuid).ToHashSet();
        return manifest.Entities.Where(value => value.EntityId.IsValid
            && value.Domain != WorldDomain.AIMP && scripts.Contains(value.SourceCompositeShortGuid)
            && (value.Flags & WorldEntityFlags.RuntimeAddressable) != 0
            && (survival
                ? value.Domain == WorldDomain.World
                  && (value.Kind is WorldEntityKind.CompositeInstance or WorldEntityKind.Function)
                  && (value.PrimitiveName is not "Interaction" and not "PlayerTriggerBox"
                      and not "PlayerTriggerSphere")
                : value.Kind == WorldEntityKind.CompositeInstance
                  || value.PrimitiveName is "PlayerTriggerBox" or "PlayerTriggerSphere" or "CAGEAnimation"))
            .ToDictionary(value => value.EntityId);
    }

    private static bool IsSurvivalMissionScript(string name)
    {
        var normalized = name.Replace('/', '\\').TrimStart('\\');
        if (!normalized.StartsWith("DLC\\ChallengeMap", StringComparison.OrdinalIgnoreCase))
            return false;
        var separator = normalized.LastIndexOf('\\');
        return separator >= 0 && normalized[(separator + 1)..].StartsWith(
            "MISSION_", StringComparison.OrdinalIgnoreCase);
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
