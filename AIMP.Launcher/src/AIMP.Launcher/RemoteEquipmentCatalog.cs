using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Launcher;

internal sealed record RemoteEquipmentDefinition(
    string Id,
    string Label,
    string CompositePath,
    bool SelectorEnabled = true,
    string? DisabledReason = null);

internal static class RemoteEquipmentDefinitions
{
    internal static IReadOnlyList<RemoteEquipmentDefinition> All { get; } =
        new[]
        {
            new RemoteEquipmentDefinition("npc-motion-tracker", "Motion Tracker (NPC)", @"Required_Assets\Archetypes\Equipment\NPC_Motion_Tracker"),
            new RemoteEquipmentDefinition("npc-tracker-1979", "Tracker 1979 (NPC)", @"Required_Assets\Archetypes\Equipment\NPC_Tracker_1979"),
            new RemoteEquipmentDefinition(
                "cutting-tool", "Cutting Tool", @"Required_Assets\Archetypes\Equipment\Cutting_Tool", false,
                "The root archetype does not expose a public equipment reference suitable for the external WEAPON_GiveToCharacter bridge."),
            new RemoteEquipmentDefinition("detonator", "Detonator", @"Required_Assets\Archetypes\Equipment\Detonator"),
            new RemoteEquipmentDefinition("flare-held", "Flare", @"Required_Assets\Archetypes\Equipment\Flare_Held"),
            new RemoteEquipmentDefinition(
                "flare-held-infinite", "Flare (Infinite)", @"Required_Assets\Archetypes\Equipment\Flare_Held_Infinite", false,
                "The wrapper does not expose a unique public equipment output in the current COMMANDS graph."),
            new RemoteEquipmentDefinition(
                "medipen", "Medipen", @"Required_Assets\Archetypes\Equipment\Medipen", false,
                "Medipen is an EQUIPPABLE_ITEM with its own internal give flow; external WEAPON_GiveToCharacter provisioning is not certified."),
            new RemoteEquipmentDefinition("radio-scanner", "Radio Scanner", @"Required_Assets\Archetypes\Equipment\Radio_Scanner"),
            new RemoteEquipmentDefinition("emp-mine-held", "EMP Mine", @"Required_Assets\Archetypes\Equipment\emp_mine_held"),
            new RemoteEquipmentDefinition("explosive-mine-held", "Explosive Mine", @"Required_Assets\Archetypes\Equipment\explosive_mine_held"),
            new RemoteEquipmentDefinition("flashbang-held", "Flashbang", @"Required_Assets\Archetypes\Equipment\flashbang_held"),
            new RemoteEquipmentDefinition("incendiary-mine-held", "Incendiary Mine", @"Required_Assets\Archetypes\Equipment\incendiary_mine_held"),
            new RemoteEquipmentDefinition("large-medikit-held", "Large Medikit", @"Required_Assets\Archetypes\Equipment\large_medikit_held"),
            new RemoteEquipmentDefinition("small-medikit-held", "Small Medikit", @"Required_Assets\Archetypes\Equipment\small_medikit_held"),
            new RemoteEquipmentDefinition("molotov-held", "Molotov Cocktail", @"Required_Assets\Archetypes\Equipment\molotov_cocktail_held"),
            new RemoteEquipmentDefinition("noisemaker-held", "Noisemaker", @"Required_Assets\Archetypes\Equipment\noisemaker_held"),
            new RemoteEquipmentDefinition("pipe-bomb-held", "Pipe Bomb", @"Required_Assets\Archetypes\Equipment\pipe_bomb_held"),
            new RemoteEquipmentDefinition("smoke-bomb-held", "Smoke Bomb", @"Required_Assets\Archetypes\Equipment\smoke_bomb_held"),
            new RemoteEquipmentDefinition("tracker-1979", "Tracker 1979", @"Required_Assets\Archetypes\Equipment\Tracker_1979"),
            new RemoteEquipmentDefinition("motion-tracker", "Motion Tracker", @"Required_Assets\Archetypes\Equipment\Motion_Tracker"),
            new RemoteEquipmentDefinition("npc-pistol", "Pistol (NPC)", @"Required_Assets\Archetypes\Weapons\NPC_Pistol_Standard"),
            new RemoteEquipmentDefinition("npc-pistol-one-shot", "Pistol One-Shot (NPC)", @"Required_Assets\Archetypes\Weapons\NPC_Pistol_Special_Oneshotkill"),
            new RemoteEquipmentDefinition("npc-shotgun", "Shotgun (NPC)", @"Required_Assets\Archetypes\Weapons\NPC_Shotgun_Standard"),
            new RemoteEquipmentDefinition("npc-flamethrower", "Flamethrower (NPC)", @"Required_Assets\Archetypes\Weapons\NPC_Flamethrower_Standard"),
            new RemoteEquipmentDefinition("npc-melee", "Melee Weapon (NPC)", @"Required_Assets\Archetypes\Weapons\NPC_MeleeWeapon"),
            new RemoteEquipmentDefinition("pistol", "Pistol", @"Required_Assets\Archetypes\Weapons\Pistol_Standard"),
            new RemoteEquipmentDefinition("shotgun", "Shotgun", @"Required_Assets\Archetypes\Weapons\Shotgun_Standard"),
            new RemoteEquipmentDefinition("flamethrower", "Flamethrower", @"Required_Assets\Archetypes\Weapons\Flamethrower_Standard"),
            new RemoteEquipmentDefinition("boltgun", "Boltgun", @"Required_Assets\Archetypes\Weapons\Boltgun_Standard"),
            new RemoteEquipmentDefinition(
                "cattleprod", "Cattle Prod", @"Required_Assets\Archetypes\Weapons\Cattleprod", false,
                "The retail root wrapper does not expose its SharedScript WEAPON output as a public root output."),
            new RemoteEquipmentDefinition("melee", "Melee Weapon", @"Required_Assets\Archetypes\Weapons\MeleeWeapon")
        };

    internal static IReadOnlyList<RemoteEquipmentDefinition> Selectable { get; } =
        All.Where(static definition => definition.SelectorEnabled).ToArray();

    internal static bool TryGet(string id, out RemoteEquipmentDefinition definition)
    {
        var resolved = All.FirstOrDefault(candidate =>
            string.Equals(candidate.Id, id, StringComparison.Ordinal));
        if (resolved is null)
        {
            definition = null!;
            return false;
        }

        definition = resolved;
        return true;
    }
}

// Static CATHODE graph discovery for Character equipment archetypes.
//
// Weapon archetypes commonly bridge public SPAWN and WEAPON ports through one
// or more SharedScript composite instances. Walk those ports recursively and
// only accept one unique path where SPAWN and the public output terminate on
// the same AIMED_ITEM, AIMED_WEAPON or MELEE_WEAPON.
//
// EQUIPPABLE_ITEM is deliberately excluded. Medipen is the known example: it
// owns a different internal flow and is not a generic external Weapon binding.
internal sealed record RemoteEquipmentCatalog(
    RemoteEquipmentDefinition Definition,
    uint EquipmentEntityShortGuid,
    uint EquipmentOutputShortGuid,
    uint SpawnSchedulerParameterShortGuid,
    uint SpawnTargetInputShortGuid,
    uint EquipmentSlot,
    string CompositeName,
    string OutputName,
    string EquipmentFunctionType,
    string CommandsPath);

internal sealed record RemoteEquipmentCatalogSet(
    IReadOnlyDictionary<string, RemoteEquipmentCatalog> Available,
    IReadOnlyDictionary<string, string> Unavailable,
    string CommandsPath)
{
    internal bool TryGet(string id, out RemoteEquipmentCatalog catalog) =>
        Available.TryGetValue(id, out catalog!);
}

internal static class RemoteEquipmentCatalogCompiler
{
    private enum TraceRole : byte
    {
        Spawn,
        Output
    }

    private sealed record PortTarget(
        FunctionEntity Function,
        uint ParameterShortGuid);

    private sealed record EquipmentEndpoint(
        string PathKey,
        uint FinalEntityShortGuid,
        FunctionType FunctionType,
        uint RootTargetInputShortGuid,
        uint EquipmentSlot,
        string LeafCompositeName);

    internal static RemoteEquipmentCatalogSet CompileAll(
        string levelOrWorldPath,
        out string diagnostic)
    {
        var commandsPath = ResolveCommandsPath(levelOrWorldPath);
        var commands = LoadCommands(commandsPath);
        var available = new Dictionary<string, RemoteEquipmentCatalog>(StringComparer.Ordinal);
        var unavailable = new Dictionary<string, string>(StringComparer.Ordinal);

        foreach (var definition in RemoteEquipmentDefinitions.All)
        {
            if (!definition.SelectorEnabled)
            {
                unavailable[definition.Id] =
                    definition.DisabledReason ?? "This archetype is intentionally disabled for external provisioning.";
                continue;
            }

            var catalog = Compile(commands, commandsPath, definition, out var itemDiagnostic);
            if (catalog is null)
                unavailable[definition.Id] = itemDiagnostic;
            else
                available[definition.Id] = catalog;
        }

        diagnostic =
            $"Equipment catalog resolved {available.Count}/{RemoteEquipmentDefinitions.Selectable.Count} selectable entries " +
            $"from '{commandsPath}'. DisabledOrUnavailable={unavailable.Count}.";
        return new RemoteEquipmentCatalogSet(available, unavailable, commandsPath);
    }

    private static RemoteEquipmentCatalog? Compile(
        Commands commands,
        string commandsPath,
        RemoteEquipmentDefinition definition,
        out string diagnostic)
    {
        var matching = commands.Entries
            .Where(static composite => composite is not null)
            .Where(composite => IsCompositeName(composite.name, definition.CompositePath))
            .ToArray();

        if (matching.Length == 0)
        {
            diagnostic = $"'{definition.CompositePath}' is not present in this level's COMMANDS graph.";
            return null;
        }

        var tail = CompositeTail(definition.CompositePath);
        var exact = matching
            .Where(composite =>
                string.Equals(Normalize(composite.name), Normalize(definition.CompositePath), StringComparison.OrdinalIgnoreCase)
                || string.Equals(Normalize(composite.name), tail, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        Composite? root = exact.Length == 1
            ? exact[0]
            : exact.Length == 0 && matching.Length == 1
                ? matching[0]
                : null;
        if (root is null)
        {
            diagnostic =
                $"'{definition.CompositePath}' discovery is ambiguous " +
                $"({matching.Length} semantic composites, {exact.Length} exact names).";
            return null;
        }

        var spawnName = ShortGuidUtils.Generate("SPAWN").AsUInt32;
        var spawnVariables = root.GetEntities()
            .OfType<VariableEntity>()
            .Where(variable => variable.name.AsUInt32 == spawnName)
            .ToArray();
        if (spawnVariables.Length != 1)
        {
            diagnostic = $"'{root.name}' exposes {spawnVariables.Length} SPAWN variables; exactly one is required.";
            return null;
        }

        var spawnEndpoints = TracePublicPort(
            commands,
            root,
            spawnName,
            TraceRole.Spawn,
            new HashSet<uint>(),
            Array.Empty<uint>(),
            null);
        if (spawnEndpoints.Count == 0)
        {
            diagnostic =
                $"'{root.name}' SPAWN does not terminate on a supported AIMED_ITEM/AIMED_WEAPON/MELEE_WEAPON node.";
            return null;
        }

        var candidates = new List<(EquipmentEndpoint Spawn, EquipmentEndpoint Output, VariableEntity OutputVariable)>();
        foreach (var outputVariable in root.GetEntities()
                     .OfType<VariableEntity>()
                     .Where(variable => variable.shortGUID.AsUInt32 != spawnVariables[0].shortGUID.AsUInt32))
        {
            var outputEndpoints = TracePublicPort(
                commands,
                root,
                outputVariable.name.AsUInt32,
                TraceRole.Output,
                new HashSet<uint>(),
                Array.Empty<uint>(),
                null);

            foreach (var spawnEndpoint in spawnEndpoints)
            foreach (var outputEndpoint in outputEndpoints)
            {
                if (spawnEndpoint.FinalEntityShortGuid != outputEndpoint.FinalEntityShortGuid ||
                    spawnEndpoint.FunctionType != outputEndpoint.FunctionType ||
                    spawnEndpoint.EquipmentSlot != outputEndpoint.EquipmentSlot ||
                    !string.Equals(spawnEndpoint.PathKey, outputEndpoint.PathKey, StringComparison.Ordinal))
                    continue;

                candidates.Add((spawnEndpoint, outputEndpoint, outputVariable));
            }
        }

        var uniqueCandidates = candidates
            .GroupBy(candidate => new
            {
                candidate.Spawn.PathKey,
                candidate.Spawn.FinalEntityShortGuid,
                candidate.Spawn.FunctionType,
                candidate.Spawn.EquipmentSlot,
                Output = candidate.OutputVariable.name.AsUInt32
            })
            .Select(group => group.First())
            .ToArray();

        if (uniqueCandidates.Length != 1)
        {
            diagnostic = uniqueCandidates.Length == 0
                ? $"'{root.name}' has no unique public SPAWN/output path to a supported equipment node."
                : $"'{root.name}' has {uniqueCandidates.Length} possible public SPAWN/output equipment paths; discovery fails closed.";
            return null;
        }

        var selected = uniqueCandidates[0];
        var outputName = ShortGuidUtils.FindString(selected.OutputVariable.name);
        var typeName = selected.Spawn.FunctionType.ToString();
        diagnostic =
            $"Resolved {definition.Label}: composite='{root.name}', SPAWN=0x{spawnName:X8}, " +
            $"equipment={typeName}:0x{selected.Spawn.FinalEntityShortGuid:X8}, output='{outputName}' " +
            $"(0x{selected.OutputVariable.name.AsUInt32:X8}), slot={selected.Spawn.EquipmentSlot}, " +
            $"rootTarget=0x{selected.Spawn.RootTargetInputShortGuid:X8}, leaf='{selected.Spawn.LeafCompositeName}'.";

        return new RemoteEquipmentCatalog(
            definition,
            selected.Spawn.FinalEntityShortGuid,
            selected.OutputVariable.name.AsUInt32,
            spawnName,
            selected.Spawn.RootTargetInputShortGuid,
            selected.Spawn.EquipmentSlot,
            root.name ?? definition.CompositePath,
            outputName,
            typeName,
            commandsPath);
    }

    private static IReadOnlyList<EquipmentEndpoint> TracePublicPort(
        Commands commands,
        Composite composite,
        uint publicPortShortGuid,
        TraceRole role,
        HashSet<uint> compositeStack,
        IReadOnlyList<uint> instancePath,
        uint? rootTargetInputShortGuid)
    {
        if (!compositeStack.Add(composite.shortGUID.AsUInt32))
            return Array.Empty<EquipmentEndpoint>();

        try
        {
            var publicVariables = composite.GetEntities()
                .OfType<VariableEntity>()
                .Where(variable => variable.name.AsUInt32 == publicPortShortGuid)
                .ToArray();
            if (publicVariables.Length != 1)
                return Array.Empty<EquipmentEndpoint>();

            var finalParameter = role == TraceRole.Spawn
                ? ShortGuidUtils.Generate("spawn").AsUInt32
                : ShortGuidUtils.Generate("reference").AsUInt32;

            var results = new List<EquipmentEndpoint>();
            foreach (var target in GetPortTargets(composite, publicVariables[0]))
            {
                var firstTarget = rootTargetInputShortGuid ?? target.ParameterShortGuid;
                var function = target.Function;
                if (function.function.IsFunctionType)
                {
                    var functionType = function.function.AsFunctionType;
                    if (!IsSupportedEquipmentFunction(functionType) ||
                        target.ParameterShortGuid != finalParameter)
                        continue;

                    var slotParameter = function.GetParameter("equipment_slot");
                    if (slotParameter?.content is not cEnum slot ||
                        slot.enumID.AsUInt32 != ShortGuidUtils.Generate("EQUIPMENT_SLOT").AsUInt32 ||
                        slot.enumIndex is < 0 or > 10)
                        continue;

                    var fullPath = instancePath
                        .Concat(new[] { function.shortGUID.AsUInt32 })
                        .ToArray();
                    results.Add(new EquipmentEndpoint(
                        string.Join("/", fullPath.Select(static value => value.ToString("X8"))),
                        function.shortGUID.AsUInt32,
                        functionType,
                        firstTarget,
                        checked((uint)slot.enumIndex),
                        composite.name ?? composite.shortGUID.ToString()));
                    continue;
                }

                var children = commands.Entries
                    .Where(static child => child is not null)
                    .Where(child => child.shortGUID.AsUInt32 == function.function.AsUInt32)
                    .ToArray();
                if (children.Length != 1)
                    continue;

                var nestedPath = instancePath
                    .Concat(new[] { function.shortGUID.AsUInt32 })
                    .ToArray();
                results.AddRange(TracePublicPort(
                    commands,
                    children[0],
                    target.ParameterShortGuid,
                    role,
                    compositeStack,
                    nestedPath,
                    firstTarget));
            }

            return results
                .GroupBy(endpoint => new
                {
                    endpoint.PathKey,
                    endpoint.FinalEntityShortGuid,
                    endpoint.FunctionType,
                    endpoint.RootTargetInputShortGuid,
                    endpoint.EquipmentSlot
                })
                .Select(group => group.First())
                .ToArray();
        }
        finally
        {
            compositeStack.Remove(composite.shortGUID.AsUInt32);
        }
    }

    private static IReadOnlyList<PortTarget> GetPortTargets(
        Composite composite,
        VariableEntity publicVariable)
    {
        var functions = composite.GetEntities()
            .OfType<FunctionEntity>()
            .ToDictionary(function => function.shortGUID.AsUInt32);

        var targets = new List<PortTarget>();

        foreach (var link in publicVariable.childLinks)
        {
            if (link.thisParamID.AsUInt32 != publicVariable.name.AsUInt32 ||
                !functions.TryGetValue(link.linkedEntityID.AsUInt32, out var function))
                continue;

            targets.Add(new PortTarget(function, link.linkedParamID.AsUInt32));
        }

        foreach (var function in functions.Values)
        foreach (var link in function.childLinks)
        {
            if (link.linkedEntityID.AsUInt32 != publicVariable.shortGUID.AsUInt32 ||
                link.linkedParamID.AsUInt32 != publicVariable.name.AsUInt32)
                continue;

            targets.Add(new PortTarget(function, link.thisParamID.AsUInt32));
        }

        return targets
            .GroupBy(target => new
            {
                Function = target.Function.shortGUID.AsUInt32,
                target.ParameterShortGuid
            })
            .Select(group => group.First())
            .ToArray();
    }

    private static bool IsSupportedEquipmentFunction(FunctionType functionType) =>
        functionType == FunctionType.AIMED_ITEM ||
        functionType == FunctionType.AIMED_WEAPON ||
        functionType == FunctionType.MELEE_WEAPON;

    private static bool IsCompositeName(string? value, string expectedPath)
    {
        if (string.IsNullOrWhiteSpace(value)) return false;
        var normalized = Normalize(value);
        var expected = Normalize(expectedPath);
        var tail = CompositeTail(expectedPath);
        return string.Equals(normalized, expected, StringComparison.OrdinalIgnoreCase)
            || string.Equals(normalized, tail, StringComparison.OrdinalIgnoreCase)
            || normalized.EndsWith("\\" + tail, StringComparison.OrdinalIgnoreCase);
    }

    private static string Normalize(string? value) =>
        (value ?? string.Empty).Replace('/', '\\').TrimEnd('\\');

    private static string CompositeTail(string value)
    {
        var normalized = Normalize(value);
        var separator = normalized.LastIndexOf('\\');
        return separator < 0 ? normalized : normalized[(separator + 1)..];
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
