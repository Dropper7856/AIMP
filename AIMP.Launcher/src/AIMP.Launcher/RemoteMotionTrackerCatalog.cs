using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Launcher;

// Static CATHODE graph discovery for the third-person Motion Tracker.
// The graph proves the unique AIMED_ITEM associated with the public
// MOTION_TRACKER variable and the exact public SPAWN edge that activates it.
// Runtime ownership and ObjectRef construction stay entirely in AIMP.Native.
// MotionTrackerOutputShortGuid is the PUBLIC MOTION_TRACKER port queried on
// the instantiated composite. The inner AIMED_ITEM.reference identity is still
// independently certified by this graph and checked against the resolved payload.
internal sealed record RemoteMotionTrackerCatalog(
    uint AimedItemEntityShortGuid,
    uint MotionTrackerOutputShortGuid,
    uint SpawnSchedulerParameterShortGuid,
    uint SpawnTargetInputShortGuid,
    string CompositeName,
    string CommandsPath);

internal static class RemoteMotionTrackerCatalogCompiler
{
    private const string MotionTrackerCompositeName = "NPC_Motion_Tracker";

    internal static RemoteMotionTrackerCatalog? Compile(
        string levelOrWorldPath,
        out string diagnostic)
    {
        var commandsPath = ResolveCommandsPath(levelOrWorldPath);
        var commands = LoadCommands(commandsPath);

        var matching = commands.Entries
            .Where(static composite => composite is not null)
            .Where(composite => IsMotionTrackerCompositeName(composite.name))
            .ToArray();

        if (matching.Length == 0)
        {
            diagnostic = "NPC_Motion_Tracker is not present in this level's COMMANDS graph.";
            return null;
        }

        var exact = matching
            .Where(composite => string.Equals(
                composite.name, MotionTrackerCompositeName,
                StringComparison.OrdinalIgnoreCase))
            .ToArray();
        Composite? root = exact.Length == 1
            ? exact[0]
            : exact.Length == 0 && matching.Length == 1
                ? matching[0]
                : null;
        if (root is null)
        {
            diagnostic =
                $"NPC_Motion_Tracker discovery is ambiguous " +
                $"({matching.Length} semantic composites, {exact.Length} exact names).";
            return null;
        }

        var entities = root.GetEntities().ToArray();
        var spawnName = ShortGuidUtils.Generate("SPAWN").AsUInt32;
        var motionTrackerName = ShortGuidUtils.Generate("MOTION_TRACKER").AsUInt32;
        var referenceParam = ShortGuidUtils.Generate("reference").AsUInt32;

        var spawnVariables = entities
            .OfType<VariableEntity>()
            .Where(variable => variable.name.AsUInt32 == spawnName)
            .ToArray();
        var motionVariables = entities
            .OfType<VariableEntity>()
            .Where(variable => variable.name.AsUInt32 == motionTrackerName)
            .ToArray();
        if (spawnVariables.Length != 1 || motionVariables.Length != 1)
        {
            diagnostic =
                $"'{root.name}' does not expose the unique SPAWN/MOTION_TRACKER interface " +
                $"required by the NPC Motion Tracker contract " +
                $"(spawn={spawnVariables.Length}, motionTracker={motionVariables.Length}).";
            return null;
        }

        var spawnVariable = spawnVariables[0];
        var motionVariable = motionVariables[0];
        var candidates = entities
            .OfType<FunctionEntity>()
            .Where(function => function.function.IsFunctionType
                && function.function.AsFunctionType == FunctionType.AIMED_ITEM)
            .Where(function => IsReferenceExposed(function, motionVariable, referenceParam))
            .Where(function => IsSpawnConnected(spawnVariable, function))
            .ToArray();

        if (candidates.Length != 1)
        {
            diagnostic = candidates.Length == 0
                ? $"'{root.name}' has no AIMED_ITEM wired as SPAWN -> AIMED_ITEM.reference -> MOTION_TRACKER."
                : $"'{root.name}' has {candidates.Length} AIMED_ITEM candidates for MOTION_TRACKER; discovery fails closed.";
            return null;
        }

        var selected = candidates[0];

        // Require a unique public reference bridge in either serialization direction.
        var forwardReferenceLinks = selected.childLinks
            .Where(link =>
                link.thisParamID.AsUInt32 == referenceParam
                && link.linkedEntityID.AsUInt32 == motionVariable.shortGUID.AsUInt32)
            .ToArray();
        var reverseReferenceLinks = motionVariable.childLinks
            .Where(link =>
                link.linkedEntityID.AsUInt32 == selected.shortGUID.AsUInt32
                && link.linkedParamID.AsUInt32 == referenceParam)
            .ToArray();
        if (forwardReferenceLinks.Length + reverseReferenceLinks.Length != 1)
        {
            diagnostic =
                $"'{root.name}' has {forwardReferenceLinks.Length} AIMED_ITEM.reference -> MOTION_TRACKER links and " +
                $"{reverseReferenceLinks.Length} MOTION_TRACKER -> AIMED_ITEM.reference links; " +
                "the Motion Tracker association must be unique and discovery fails closed.";
            return null;
        }
        var publicMotionTrackerOutputShortGuid = motionVariable.name.AsUInt32;
        var referenceBridgeDirection = forwardReferenceLinks.Length == 1
            ? "AIMED_ITEM.reference->MOTION_TRACKER"
            : "MOTION_TRACKER->AIMED_ITEM.reference";

        // Validate and retain the concrete SPAWN -> AIMED_ITEM edge. The runtime
        // diagnostics proved that creating/finalizing the dynamic Composite does not
        // make the AIMED_ITEM usable by itself; Native must execute this exact retail
        // edge once before GiveToCharacter. These GUIDs remain map-local Launcher ->
        // Native data and are never put on the multiplayer wire.
        var spawnLinks = spawnVariable.childLinks
            .Where(link =>
                link.linkedEntityID.AsUInt32 == selected.shortGUID.AsUInt32
                && link.thisParamID.AsUInt32 != 0U
                && link.linkedParamID.AsUInt32 != 0U)
            .ToArray();
        if (spawnLinks.Length != 1)
        {
            diagnostic =
                $"'{root.name}' has {spawnLinks.Length} concrete SPAWN -> AIMED_ITEM input bridges; " +
                "the remote equipment bridge requires exactly one and fails closed.";
            return null;
        }

        var spawnLink = spawnLinks[0];
        // Native dispatches the public input via the root's virtual +0x10C.
        // Do not substitute the AIMED_ITEM child input for that public port.
        if (spawnLink.thisParamID.AsUInt32 != spawnVariable.name.AsUInt32)
        {
            diagnostic =
                $"'{root.name}' exposes SPAWN=0x{spawnVariable.name.AsUInt32:X8} but its unique " +
                $"SPAWN bridge uses source parameter 0x{spawnLink.thisParamID.AsUInt32:X8}; " +
                "The native public-input contract requires the certified direct public-port identity and fails closed.";
            return null;
        }

        diagnostic =
            $"Resolved {root.name} reference pipeline: " +
            $"SPAWN=0x{spawnVariable.name.AsUInt32:X8} " +
            $"[scheduler=0x{spawnLink.thisParamID.AsUInt32:X8}, " +
            $"targetInput=0x{spawnLink.linkedParamID.AsUInt32:X8}] -> " +
            $"AIMED_ITEM=0x{selected.shortGUID.AsUInt32:X8}.reference " +
            $"[publicOutput=0x{publicMotionTrackerOutputShortGuid:X8}, bridge={referenceBridgeDirection}] <-> " +
            $"MOTION_TRACKER=0x{motionVariable.name.AsUInt32:X8} " +
            $"[VariableEntity=0x{motionVariable.shortGUID.AsUInt32:X8}].";
        return new RemoteMotionTrackerCatalog(
            selected.shortGUID.AsUInt32,
            publicMotionTrackerOutputShortGuid,
            spawnLink.thisParamID.AsUInt32,
            spawnLink.linkedParamID.AsUInt32,
            root.name ?? MotionTrackerCompositeName,
            commandsPath);
    }

    private static bool IsReferenceExposed(
        FunctionEntity function,
        VariableEntity publicReference,
        uint referenceParam)
    {
        return function.childLinks.Any(link =>
                   link.thisParamID.AsUInt32 == referenceParam
                   && link.linkedEntityID.AsUInt32 == publicReference.shortGUID.AsUInt32)
            || publicReference.childLinks.Any(link =>
                   link.linkedEntityID.AsUInt32 == function.shortGUID.AsUInt32
                   && link.linkedParamID.AsUInt32 == referenceParam);
    }

    private static bool IsSpawnConnected(
        VariableEntity spawnVariable,
        FunctionEntity function)
    {
        return spawnVariable.childLinks.Any(link =>
                   link.linkedEntityID.AsUInt32 == function.shortGUID.AsUInt32)
            || function.childLinks.Any(link =>
                   link.linkedEntityID.AsUInt32 == spawnVariable.shortGUID.AsUInt32);
    }

    private static bool IsMotionTrackerCompositeName(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return false;
        var normalized = value.Replace('/', '\\').TrimEnd('\\');
        return string.Equals(normalized, MotionTrackerCompositeName,
                   StringComparison.OrdinalIgnoreCase)
            || normalized.EndsWith("\\" + MotionTrackerCompositeName,
                StringComparison.OrdinalIgnoreCase);
    }

    private static string ResolveCommandsPath(string levelOrWorldPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(levelOrWorldPath);
        if (File.Exists(levelOrWorldPath)) return Path.GetFullPath(levelOrWorldPath);
        if (!Directory.Exists(levelOrWorldPath))
            throw new DirectoryNotFoundException(levelOrWorldPath);

        var root = Path.GetFullPath(levelOrWorldPath);
        var candidates = new[]
        {
            Path.Combine(root, "COMMANDS.PAK"),
            Path.Combine(root, "COMMANDS.BIN"),
            Path.Combine(root, "WORLD", "COMMANDS.PAK"),
            Path.Combine(root, "WORLD", "COMMANDS.BIN")
        };
        foreach (var candidate in candidates)
            if (File.Exists(candidate)) return candidate;
        throw new FileNotFoundException(
            $"No COMMANDS.PAK or COMMANDS.BIN was found below '{root}'.");
    }

    private static Commands LoadCommands(string path)
    {
        // Keep this equivalent to WorldManifestCompiler.LoadCommands: equipment
        // discovery needs graph structure only and must not require optional
        // cross-file resource databases to exist.
        var placeholderRoot = Path.Combine(
            Path.GetTempPath(), "AIMP.RemoteEquipmentCatalog", Guid.NewGuid().ToString("N"));
        var environmentAnimations = new EnvironmentAnimations(
            Path.Combine(placeholderRoot, "ENVIRONMENT_ANIMATION.DAT"),
            null!);
        var collisionMaps = new CollisionMaps(
            Path.Combine(placeholderRoot, "COLLISION.MAP"),
            null!,
            null!);
        var renderableElements = new RenderableElements(
            Path.Combine(placeholderRoot, "REDS.BIN"),
            null!,
            null!);

        var commands = new Commands(path, environmentAnimations, collisionMaps, renderableElements);
        if (commands.Entries.Count == 0)
            throw new InvalidDataException($"CathodeLib loaded zero composites from '{path}'.");
        commands.ClearReferences();
        return commands;
    }
}
