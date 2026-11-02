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
    // Survival progression is compiled from the manifest's authored graph.
    // These are reusable CATHODE signal *families*, not level names or a list
    // of hardcoded map objectives. Unknown output names can also qualify when
    // their verified targets include a semantic objective/mission-complete sink.
    private static readonly HashSet<uint> SurvivalCompletionPorts = new[]
    {
        "complete", "completed", "finished", "success",
        "objectiveComplete", "objectiveCompleted", "objective_complete",
        "missionComplete", "missionCompleted", "mission_complete",
        "levelComplete", "levelCompleted", "level_complete"
    }.Concat(Enumerable.Range(1, 64).SelectMany(index => new[]
    {
        $"obj{index}Completed", $"obj{index}Complete",
        $"objective{index}Completed", $"objective{index}Complete",
        $"objective_{index}_complete", $"objective_{index}_completed"
    })).Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // These events can represent the interacting Character or an animation
    // rather than a portable milestone. Never suppress/replay them in Survival.
    private static readonly HashSet<uint> SurvivalLocalInteractionPorts = new[]
    {
        "used", "interact", "interaction", "on_entered", "on_exited",
        "animation_finished", "pressed", "activated", "deactivated",
        "entered", "exited", "start", "started"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // Only stateless control inputs are replayable without the sender-local
    // callback/Character. Indexed objective inputs are a *generic* convention.
    private static readonly HashSet<uint> SurvivalControlInputs = new[]
    {
        "complete", "completed", "finished", "success",
        "objectiveComplete", "objectiveCompleted", "objective_complete",
        "missionComplete", "missionCompleted", "mission_complete",
        "levelComplete", "levelCompleted", "level_complete",
        "gotoEndScreen", "showSplash", "setObjIcons"
    }.Concat(Enumerable.Range(1, 64).SelectMany(index => new[]
    {
        $"Objective {index} Complete", $"obj{index}Completed",
        $"obj{index}Complete", $"objective{index}Completed",
        $"objective{index}Complete"
    })).Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

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
        var composites = manifest.Composites.ToDictionary(value => value.CompositeShortGuid);
        var sources = MissionSources(manifest, survival);
        var roots = manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => sources.TryGetValue(group.Key.SourceEntity, out var source)
                && group.All(edge => SafeTarget(edge, entities, survival))
                && (!survival
                    ? CompletionPorts.Contains(group.Key.SourceParameter)
                    : SurvivalBoundary(group.Key.SourceParameter, source, group, entities, composites)))
            .Select(group => new MissionSyncFrontier(sources[group.Key.SourceEntity], group.Key.SourceParameter))
            .OrderBy(value => value.Source.EntityId.High).ThenBy(value => value.Source.EntityId.Low)
            .ThenBy(value => value.Signal).ToArray();
        if (roots.Length > MissionSignalValidation.MaximumSignalsPerGeneration)
            throw new InvalidOperationException("MissionSync has too many authored mission frontiers.");
        return roots;
    }

    private static bool SurvivalBoundary(
        uint output, WorldEntityDescriptor source,
        IGrouping<(WorldEntityId SourceEntity, uint SourceParameter), WorldConnectionDescriptor> edges,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities,
        IReadOnlyDictionary<uint, WorldCompositeDescriptor> composites)
    {
        if (SurvivalLocalInteractionPorts.Contains(output)) return false;

        var sourceInMission = composites.TryGetValue(source.SourceCompositeShortGuid, out var composite)
            && IsMissionGraph(composite.Name);
        // Known completion outputs need to originate from an authored mission
        // graph, never a generator, player interaction or arbitrary World script.
        if (sourceInMission && SurvivalCompletionPorts.Contains(output)) return true;

        // Custom/unnamed output conventions can be admitted when the actual
        // fan-out proves a progression sink. This uses entity semantics, not
        // a DLC path, map ID or hardcoded output name.
        return edges.Any(edge => entities.TryGetValue(edge.TargetEntity, out var target)
            && IsObjectiveSink(target));
    }

    private static bool IsObjectiveSink(WorldEntityDescriptor target)
    {
        if (target.Domain is not (WorldDomain.World or WorldDomain.UI)) return false;
        // A mission's entire graph is not an objective sink. Require a
        // specific target primitive/entity that describes the outcome.
        return IsObjectiveName(target.PrimitiveName) || IsObjectiveName(target.EntityName);
    }

    private static bool IsObjectiveName(string? name) =>
        !string.IsNullOrWhiteSpace(name) && (
            name.Contains("Objective", StringComparison.OrdinalIgnoreCase)
            || name.Contains("MissionComplete", StringComparison.OrdinalIgnoreCase)
            || name.Contains("LevelComplete", StringComparison.OrdinalIgnoreCase)
            || name.Contains("MissionEnd", StringComparison.OrdinalIgnoreCase));

    private static bool SafeTarget(
        WorldConnectionDescriptor edge,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities,
        bool survival)
    {
        if (!edge.TargetEntity.IsValid || !entities.TryGetValue(edge.TargetEntity, out var target)
            || target.Domain == WorldDomain.AIMP) return false;
        if (!ControlInputs.Contains(edge.TargetParameter)
            && !(survival && SurvivalControlInputs.Contains(edge.TargetParameter))) return false;
        // Surviving mission fan-out must never replay the Character's local
        // movement, a proximity trigger, an AI action or unknown data binding.
        return !survival ||
            (target.Domain is WorldDomain.World or WorldDomain.UI)
            && target.PrimitiveName is not ("Interaction" or "PlayerTriggerBox" or "PlayerTriggerSphere");
    }

    // Campaign keeps its existing SCRIPT_StoryMission frontier contract.
    // Survival identifies the enclosing *mission semantics* across any DLC,
    // custom challenge or future map. It also accepts any graph with a
    // proved objective sink (see SurvivalBoundary), regardless of its name.
    private static Dictionary<WorldEntityId, WorldEntityDescriptor> MissionSources(
        WorldManifest manifest, bool survival)
    {
        var scripts = manifest.Composites.Where(value => !value.AimpNamespace
            && (!survival
                ? value.Name.Replace('/', '\\').StartsWith("SCRIPT_StoryMission\\",
                    StringComparison.OrdinalIgnoreCase)
                : IsMissionGraph(value.Name)))
            .Select(value => value.CompositeShortGuid).ToHashSet();
        return manifest.Entities.Where(value => value.EntityId.IsValid
            && value.Domain != WorldDomain.AIMP
            && (survival || scripts.Contains(value.SourceCompositeShortGuid))
            && (value.Flags & WorldEntityFlags.RuntimeAddressable) != 0
            && (survival
                ? value.Domain == WorldDomain.World
                  && (value.Kind is WorldEntityKind.CompositeInstance or WorldEntityKind.Function)
                  && value.PrimitiveName is not ("Interaction" or "PlayerTriggerBox" or "PlayerTriggerSphere")
                : value.Kind == WorldEntityKind.CompositeInstance
                  || value.PrimitiveName is "PlayerTriggerBox" or "PlayerTriggerSphere" or "CAGEAnimation"))
            .ToDictionary(value => value.EntityId);
    }

    private static bool IsMissionGraph(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return false;
        // Match a reusable semantic token rather than an arbitrary substring:
        // "Permission" is not a mission. Recognizes StoryMission, MISSION_X,
        // MissionControl and lowercase "mission" across arbitrary paths.
        var tokens = name.Replace('/', '\\').Split(['\\', '_', '-', '.'],
            StringSplitOptions.RemoveEmptyEntries);
        return tokens.Any(token =>
            token.Equals("mission", StringComparison.OrdinalIgnoreCase)
            || token.StartsWith("mission", StringComparison.OrdinalIgnoreCase)
            || token.EndsWith("Mission", StringComparison.Ordinal));
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
