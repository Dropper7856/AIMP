using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AIMP.Protocol;
using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Launcher;

internal sealed record WorldManifestCompilerProgress(
    int Percentage,
    string Stage);

internal sealed record WorldManifestCompilationResult(
    WorldManifest Manifest,
    string BinaryPath,
    string? JsonPath,
    bool CacheHit);

internal sealed class WorldManifestCompiler
{
    private const int MaxCompositeDepth = 128;
    private const string EntityHashTag = "AIMP-WORLD-ENTITY-V2";
    private const string MapHashTag = "AIMP-WORLDMANIFEST-MAP-V2";
    private const string HierarchyHashTag = "AIMP-WORLD-HIERARCHY-V2";
    private const string ZoneHashTag = "AIMP-WORLD-ZONE-V2";
    // STATIC CONFIRMED retail ShortGuid("set_false"). BuildFix05 uses this
    // generic primitive boundary only; no Door/Unlock semantic is encoded.
    private const uint LogicSwitchSetFalseInputShortGuid = 0xD2B3CC2BU;
    private const int MaxCompositePublicInputCanonicalAnchors = 4_000_000;
    private const int MaxCausalProfileReachabilityNodes = 1024;
    private const int MaxCausalProfileDepth = 64;

    private readonly List<string> _warnings = new();
    private readonly Dictionary<string, uint> _pathIds = new(StringComparer.Ordinal);
    private readonly List<WorldInstancePathDescriptor> _paths = new();
    private readonly List<WorldEntityDescriptor> _entities = new();
    private readonly List<WorldCompositePublicInputDescriptor> _compositePublicInputs = new();
    private readonly List<WorldCompositePublicInputCanonicalAnchor> _compositePublicInputCanonicalAnchors = new();
    private readonly List<WorldCompositePublicInputCausalProfile> _compositePublicInputCausalProfiles = new();
    private readonly List<WorldConnectionDescriptor> _connections = new();
    private readonly List<WorldOverrideDescriptor> _overrides = new();
    private readonly List<WorldReferenceDescriptor> _references = new();
    private readonly List<WorldZoneDescriptor> _zones = new();
    private readonly HashSet<uint> _instantiatedCompositeIds = new();

    private WorldEntityId _mapContentId;
    private Commands? _compositeLookupSource;
    private Dictionary<uint, Composite> _compositeLookup = new();
    private uint _excludedAimpCompositeCount;
    private uint _excludedAimpEntityCount;

    public WorldManifestCompilationResult Compile(
        string levelOrWorldPath,
        string? outputPath = null,
        string? jsonPath = null,
        bool useCache = true,
        IProgress<WorldManifestCompilerProgress>? progress = null)
    {
        lock (GamePreparationSynchronization.Gate)
            return CompileCore(levelOrWorldPath, outputPath, jsonPath, useCache, progress);
    }

    private WorldManifestCompilationResult CompileCore(string levelOrWorldPath, string? outputPath,
        string? jsonPath, bool useCache, IProgress<WorldManifestCompilerProgress>? progress)
    {
        ReportProgress(progress, 0, "Preparing manifest compilation...");
        ResetCompilationState();

        ReportProgress(progress, 5, "Resolving level directory...");
        var worldDirectory = ResolveWorldDirectory(levelOrWorldPath);
        var pakPath = Path.Combine(worldDirectory, "COMMANDS.PAK");
        var binPath = Path.Combine(worldDirectory, "COMMANDS.BIN");
        var pakPresent = File.Exists(pakPath);
        var binPresent = File.Exists(binPath);

        if (!pakPresent && !binPresent)
        {
            throw new FileNotFoundException($"No COMMANDS.PAK or COMMANDS.BIN was found in '{worldDirectory}'.");
        }

        ReportProgress(progress, 10, "Hashing COMMANDS sources...");
        var pakHash = pakPresent ? HashFile(pakPath) : string.Empty;
        var binHash = binPresent ? HashFile(binPath) : string.Empty;
        // Entity identity no longer depends on the COMMANDS file hashes: the preparation step rewrites
        // those files per installation, so two untouched copies of a level ended up with different
        // entity ids and every replicated world mutation was rejected (ManifestRejected).
        // Original (hash-based) identity, kept for reference:
        // _mapContentId = ComputeMapContentId(pakHash, binHash, pakPresent, binPresent);
        _mapContentId = ComputeStableMapContentId(worldDirectory, pakPresent, binPresent);

        var cachePath = GetDefaultCachePath(_mapContentId);
        outputPath ??= useCache
            ? cachePath
            : Path.Combine(worldDirectory, $"WORLD_MANIFEST_{_mapContentId}.aimpwm");

        if (useCache && File.Exists(cachePath))
        {
            var cached = TryLoadCurrentCache(cachePath, pakHash, binHash, pakPresent, binPresent);
            if (cached is not null)
            {
                ReportProgress(progress, 92, "Valid manifest cache found...");
                if (!PathsEqual(cachePath, outputPath)) WorldManifestBinaryCodec.Write(outputPath, cached);
                if (!string.IsNullOrWhiteSpace(jsonPath)) WriteJson(jsonPath, cached);
                ReportProgress(progress, 100, "Manifest ready (cache hit).");
                return new WorldManifestCompilationResult(cached, outputPath, jsonPath, CacheHit: true);
            }
        }

        ReportProgress(progress, 18, pakPresent
            ? "Loading COMMANDS.PAK..."
            : "COMMANDS.PAK not present; using COMMANDS.BIN...");
        var pakCommands = pakPresent ? LoadCommands(pakPath) : null;
        ReportProgress(progress, 28, binPresent
            ? "Loading COMMANDS.BIN..."
            : "COMMANDS.BIN not present; using COMMANDS.PAK...");
        var binCommands = binPresent ? LoadCommands(binPath) : null;
        var primary = pakCommands ?? binCommands
            ?? throw new InvalidOperationException("Unable to load a COMMANDS source.");

        ReportProgress(progress, 38, "Verifying COMMANDS structure...");
        var pakFingerprint = pakCommands is null ? string.Empty : ComputeStructuralFingerprint(pakCommands);
        var binFingerprint = binCommands is null ? string.Empty : ComputeStructuralFingerprint(binCommands);
        var structurallyAgree = pakCommands is not null && binCommands is not null &&
            string.Equals(pakFingerprint, binFingerprint, StringComparison.OrdinalIgnoreCase);

        if (pakCommands is null || binCommands is null)
        {
            _warnings.Add("Only one COMMANDS source is present; cross-source structural verification was not possible.");
        }
        else if (!structurallyAgree)
        {
            _warnings.Add("COMMANDS.PAK and COMMANDS.BIN structural fingerprints differ. The PAK structure is authoritative for this manifest while BIN remains an audit source.");
        }

        var source = new WorldManifestSourceInfo(
            pakHash,
            binHash,
            pakPresent,
            binPresent,
            structurallyAgree,
            pakFingerprint,
            binFingerprint);

        var entryPoints = primary.EntryPoints ?? Array.Empty<Composite>();
        if (entryPoints.Length < 3)
        {
            _warnings.Add($"Expected three CATHODE entry points but found {entryPoints.Length}.");
        }

        for (var index = 0; index < entryPoints.Length && index < 3; index++)
        {
            var composite = entryPoints[index];
            if (composite is null) continue;

            var traverseProgress = 48 + ((index + 1) * 9);
            ReportProgress(progress, traverseProgress,
                $"Analyzing CATHODE entry point {index + 1}/{Math.Min(entryPoints.Length, 3)}...");

            // Keep the complete static graph for all COMMANDS entry points, but only
            // mark identities runtime-addressable when the instance-ID convention is
            // proven. CathodeLib's Instancing path explicitly roots EntryPoints[0] at
            // ShortGuid.InstanceGuid and derives children with
            // GenerateCompositeInstanceID(false). No equivalent anchor is assumed here
            // for EntryPoints[1]/[2].
            var rootPath = new List<uint>();
            var manifestNamespaceId = checked((uint)index + 1U);
            var runtimeBaseInstanceId = index == 0
                ? WorldManifestRuntimeIdentity.RootInstanceBaseId
                : 0U;
            if (runtimeBaseInstanceId == 0U)
            {
                _warnings.Add($"COMMANDS entry point {index} ('{composite.name}') is retained as static metadata only; no runtime composite-instance anchor is assumed without evidence.");
            }
            TraverseComposite(primary, composite, rootPath, manifestNamespaceId, runtimeBaseInstanceId, ownerCompositeInstance: default, aimpAncestor: false, new HashSet<uint>(), 0);
        }

        // BuildFix05 is entirely compiler-side. It derives a compact, fail-closed
        // reverse canonicalization index from the complete current COMMANDS graph.
        // No runtime scan is required by the injected DLL.
        ReportProgress(progress, 78, "Building public Composite input indexes...");
        BuildCompositePublicInputCanonicalAnchors();
        ReportProgress(progress, 81, "Building generic causal-boundary profiles...");
        BuildCompositePublicInputCausalProfiles();

        ReportProgress(progress, 84, "Assembling manifest descriptors...");
        var sourceFlags = WorldEntityFlags.None;
        if (pakPresent && (!binPresent || structurallyAgree)) sourceFlags |= WorldEntityFlags.SourceFromPak;
        if (binPresent && (!pakPresent || structurallyAgree)) sourceFlags |= WorldEntityFlags.SourceFromBin;
        if (!structurallyAgree) sourceFlags |= pakPresent ? WorldEntityFlags.SourceFromPak : WorldEntityFlags.SourceFromBin;
        if (sourceFlags != WorldEntityFlags.None)
        {
            for (var index = 0; index < _entities.Count; index++)
            {
                _entities[index] = _entities[index] with { Flags = _entities[index].Flags | sourceFlags };
            }
        }

        var composites = primary.Entries
            .Where(static value => value is not null)
            .OrderBy(static value => value.shortGUID.AsUInt32)
            .Select(value =>
            {
                var aimp = IsAimpCompositeName(value.name);
                return new WorldCompositeDescriptor(
                    value.shortGUID.AsUInt32,
                    value.name ?? string.Empty,
                    aimp ? WorldDomain.AIMP : ClassifyCompositeDomain(value.name),
                    aimp,
                    _instantiatedCompositeIds.Contains(value.shortGUID.AsUInt32));
            })
            .ToArray();

        var root = EntryGuid(entryPoints, 0);
        var global = EntryGuid(entryPoints, 1);
        var pause = EntryGuid(entryPoints, 2);

        var manifest = new WorldManifest(
            WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion,
            _mapContentId.High,
            _mapContentId.Low,
            root,
            global,
            pause,
            source,
            composites,
            _paths.OrderBy(static value => value.PathId).ToArray(),
            _entities
                .OrderBy(static value => value.EntityId.High)
                .ThenBy(static value => value.EntityId.Low)
                .ThenBy(static value => value.SourceCompositeShortGuid)
                .ThenBy(static value => value.SourceShortGuid)
                .ThenBy(static value => value.InstancePathId)
                .ToArray(),
            _compositePublicInputs
                .OrderBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ThenBy(static value => value.InternalTargetEntity.High)
                .ThenBy(static value => value.InternalTargetEntity.Low)
                .ThenBy(static value => value.InternalTargetInputShortGuid)
                .ThenBy(static value => value.InterfaceVariableEntity.High)
                .ThenBy(static value => value.InterfaceVariableEntity.Low)
                .ToArray(),
            _compositePublicInputCanonicalAnchors
                .OrderBy(static value => value.InternalTargetEntity.High)
                .ThenBy(static value => value.InternalTargetEntity.Low)
                .ThenBy(static value => value.InternalTargetInputShortGuid)
                .ThenBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ToArray(),
            _compositePublicInputCausalProfiles
                .OrderBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ThenBy(static value => value.EndpointKind)
                .ToArray(),
            _connections
                .OrderBy(static value => value.SourceEntity.High)
                .ThenBy(static value => value.SourceEntity.Low)
                .ThenBy(static value => value.SourceParameter)
                .ThenBy(static value => value.TargetEntity.High)
                .ThenBy(static value => value.TargetEntity.Low)
                .ThenBy(static value => value.TargetParameter)
                .ThenBy(static value => value.PotentialCausalEdge)
                .ToArray(),
            _overrides
                .OrderBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.ParameterName)
                .ThenBy(static value => value.DataType)
                .ThenBy(static value => value.ParameterVariant)
                .ToArray(),
            _references
                .OrderBy(static value => value.OwnerEntity.High)
                .ThenBy(static value => value.OwnerEntity.Low)
                .ThenBy(static value => value.Kind)
                .ThenBy(static value => value.FunctionType)
                .ThenBy(static value => StablePathKey(value.TargetPathSteps), StringComparer.Ordinal)
                .ToArray(),
            _zones
                .OrderBy(static value => value.ZoneId)
                .ThenBy(static value => value.ZoneEntity.High)
                .ThenBy(static value => value.ZoneEntity.Low)
                .ThenBy(static value => value.InstancePathId)
                .ThenBy(static value => value.EntityName, StringComparer.Ordinal)
                .ToArray(),
            _excludedAimpCompositeCount,
            _excludedAimpEntityCount,
            _warnings.Distinct(StringComparer.Ordinal).OrderBy(static value => value, StringComparer.Ordinal).ToArray());

        ReportProgress(progress, 94, "Writing manifest...");
        if (useCache) WorldManifestBinaryCodec.Write(cachePath, manifest);
        if (!useCache || !PathsEqual(cachePath, outputPath)) WorldManifestBinaryCodec.Write(outputPath, manifest);
        if (!string.IsNullOrWhiteSpace(jsonPath)) WriteJson(jsonPath, manifest);

        ReportProgress(progress, 100, "Manifest created successfully.");
        return new WorldManifestCompilationResult(manifest, outputPath, jsonPath, CacheHit: false);
    }

    private static void ReportProgress(
        IProgress<WorldManifestCompilerProgress>? progress,
        int percentage,
        string stage)
    {
        progress?.Report(new WorldManifestCompilerProgress(
            Math.Clamp(percentage, 0, 100),
            stage));
    }

    private void TraverseComposite(
        Commands commands,
        Composite composite,
        IReadOnlyList<uint> path,
        uint manifestNamespaceId,
        uint runtimeBaseInstanceId,
        WorldEntityId ownerCompositeInstance,
        bool aimpAncestor,
        HashSet<uint> compositeStack,
        int depth)
    {
        if (depth > MaxCompositeDepth)
        {
            _warnings.Add($"Composite traversal exceeded the {MaxCompositeDepth}-level safety limit at '{composite.name}'.");
            return;
        }

        var compositeId = composite.shortGUID.AsUInt32;
        var isAimp = aimpAncestor || IsAimpCompositeName(composite.name);
        if (isAimp)
        {
            _excludedAimpCompositeCount++;
            _excludedAimpEntityCount += checked((uint)composite.GetEntities().Count);
            return;
        }

        if (!compositeStack.Add(compositeId))
        {
            _warnings.Add($"Recursive composite cycle detected at '{composite.name}' (0x{compositeId:X8}); subtree traversal stopped.");
            return;
        }

        _instantiatedCompositeIds.Add(compositeId);
        var pathId = InternPath(path, manifestNamespaceId, runtimeBaseInstanceId);
        var hierarchyHash = _paths[checked((int)pathId - 1)].HierarchyHash;

        var allLocalEntities = composite.GetEntities()
            .Where(static value => value is not null)
            .OrderBy(static value => value.shortGUID.AsUInt32)
            .ToArray();

        var excludedLocalEntityIds = new HashSet<uint>();
        foreach (var entity in allLocalEntities)
        {
            if (entity is not FunctionEntity function) continue;
            var childComposite = FindComposite(commands, function.function);
            if (childComposite is null || !IsAimpCompositeName(childComposite.name)) continue;

            excludedLocalEntityIds.Add(entity.shortGUID.AsUInt32);
            _excludedAimpEntityCount++;
        }

        var localEntities = allLocalEntities
            .Where(entity => !excludedLocalEntityIds.Contains(entity.shortGUID.AsUInt32))
            .ToArray();

        var localIds = new Dictionary<uint, WorldEntityId>();
        var localDescriptors = new Dictionary<uint, WorldEntityDescriptor>();

        // Fail-closed support for the LogicSwitch fallback: an internal
        // target/input is eligible only when COMMANDS proves that the public
        // interface link is its sole incoming local childLink. Direct observed
        // QueueInput edges do not require this exclusivity because their full
        // source/link/target tuple is matched at runtime.
        var localShortGuids = localEntities
            .Select(static candidate => candidate.shortGUID.AsUInt32)
            .ToHashSet();
        var localIncomingCounts = new Dictionary<(uint Entity, uint Input), int>();
        foreach (var source in localEntities)
        {
            foreach (var link in source.childLinks)
            {
                if (!localShortGuids.Contains(link.linkedEntityID.AsUInt32))
                    continue;
                var key = (link.linkedEntityID.AsUInt32, link.linkedParamID.AsUInt32);
                localIncomingCounts[key] = localIncomingCounts.TryGetValue(key, out var count)
                    ? checked(count + 1)
                    : 1;
            }
        }

        foreach (var entity in localEntities)
        {
            var descriptor = BuildEntityDescriptor(commands, composite, entity, pathId, hierarchyHash, path, runtimeBaseInstanceId);
            _entities.Add(descriptor);
            localIds[entity.shortGUID.AsUInt32] = descriptor.EntityId;
            localDescriptors[entity.shortGUID.AsUInt32] = descriptor;

            if (descriptor.Flags.HasFlag(WorldEntityFlags.ZonePrimitive))
            {
                _zones.Add(new WorldZoneDescriptor(descriptor.EntityId, descriptor.ZoneId, pathId, descriptor.EntityName));
            }

            if (entity is FunctionEntity function && FindComposite(commands, function.function) is not null)
            {
                foreach (var parameter in function.parameters)
                {
                    _overrides.Add(new WorldOverrideDescriptor(
                        descriptor.EntityId,
                        parameter.name.AsUInt32,
                        parameter.content is null ? 0U : (uint)parameter.content.dataType,
                        (uint)parameter.variant));
                }
            }

            if (entity is ProxyEntity proxy)
            {
                _references.Add(new WorldReferenceDescriptor(
                    descriptor.EntityId,
                    WorldReferenceKind.Proxy,
                    proxy.function.AsUInt32,
                    GetPathSteps(proxy.proxy)));
            }
            else if (entity is AliasEntity alias)
            {
                _references.Add(new WorldReferenceDescriptor(
                    descriptor.EntityId,
                    WorldReferenceKind.Alias,
                    0U,
                    GetPathSteps(alias.alias)));
            }
        }

        foreach (var entity in localEntities)
        {
            if (!localIds.TryGetValue(entity.shortGUID.AsUInt32, out var sourceId)) continue;
            var sourceDescriptor = localDescriptors[entity.shortGUID.AsUInt32];

            foreach (var link in entity.childLinks)
            {
                if (!localIds.TryGetValue(link.linkedEntityID.AsUInt32, out var targetId))
                {
                    if (excludedLocalEntityIds.Contains(link.linkedEntityID.AsUInt32)) continue;
                    _warnings.Add($"Unresolved local child link in composite '{composite.name}': 0x{entity.shortGUID.AsUInt32:X8} -> 0x{link.linkedEntityID.AsUInt32:X8}.");
                    continue;
                }

                var targetDescriptor = localDescriptors[link.linkedEntityID.AsUInt32];
                var potentialCausal = sourceDescriptor.Domain == WorldDomain.World || targetDescriptor.Domain == WorldDomain.World;
                _connections.Add(new WorldConnectionDescriptor(
                    sourceId,
                    link.thisParamID.AsUInt32,
                    targetId,
                    link.linkedParamID.AsUInt32,
                    potentialCausal));

                // STATIC CONFIRMED by CathodeLib/OpenCAGE: VariableEntity.name
                // is the parameter exposed by the containing Composite, while
                // childLinks carry thisParamID -> linkedEntityID/linkedParamID.
                // Only nested Composite instances have a stable runtime owner
                // WorldEntityId today; root-interface mappings remain static-only
                // until a root instance entity identity is independently proven.
                // Public Methods (DataType.NONE) legitimately use an invalid/zero
                // source connector parameter: the public identity is VariableEntity.name
                // and the childLink only needs a concrete linked input. Requiring a
                // nonzero thisParamID silently dropped action-like public Methods from
                // the forward owner+public-input index (including WS3 probe actions).
                if (runtimeBaseInstanceId != 0U && ownerCompositeInstance.IsValid &&
                    entity is VariableEntity variable && variable.name.AsUInt32 != 0U &&
                    (link.thisParamID.AsUInt32 != 0U || variable.type == DataType.NONE) &&
                    link.linkedParamID.AsUInt32 != 0U)
                {
                    _compositePublicInputs.Add(new WorldCompositePublicInputDescriptor(
                        ownerCompositeInstance,
                        sourceId,
                        variable.name.AsUInt32,
                        link.thisParamID.AsUInt32,
                        targetId,
                        link.linkedParamID.AsUInt32,
                        (uint)variable.type,
                        variable.type == DataType.NONE,
                        localIncomingCounts.TryGetValue(
                            (link.linkedEntityID.AsUInt32, link.linkedParamID.AsUInt32),
                            out var incomingCount) && incomingCount == 1));
                }
            }
        }

        foreach (var entity in allLocalEntities)
        {
            if (entity is not FunctionEntity function) continue;
            var childComposite = FindComposite(commands, function.function);
            if (childComposite is null) continue;

            var childPath = new List<uint>(path.Count + 1);
            childPath.AddRange(path);
            childPath.Add(function.shortGUID.AsUInt32);
            if (!localDescriptors.TryGetValue(function.shortGUID.AsUInt32, out var childOwnerDescriptor))
                continue;
            TraverseComposite(commands, childComposite, childPath, manifestNamespaceId, runtimeBaseInstanceId, childOwnerDescriptor.EntityId, aimpAncestor: false, new HashSet<uint>(compositeStack), depth + 1);
        }
    }

    private WorldEntityDescriptor BuildEntityDescriptor(
        Commands commands,
        Composite composite,
        Entity entity,
        uint pathId,
        ulong hierarchyHash,
        IReadOnlyList<uint> path,
        uint runtimeBaseInstanceId)
    {
        var kind = GetKind(commands, entity);
        var primitiveType = 0U;
        var primitiveName = kind.ToString();

        if (entity is FunctionEntity function)
        {
            primitiveType = function.function.AsUInt32;
            var childComposite = FindComposite(commands, function.function);
            if (childComposite is not null)
            {
                primitiveName = childComposite.name ?? $"Composite_0x{primitiveType:X8}";
                kind = WorldEntityKind.CompositeInstance;
            }
            else if (function.function.IsFunctionType)
            {
                primitiveName = function.function.AsFunctionType.ToString();
            }
            else
            {
                primitiveName = $"Function_0x{primitiveType:X8}";
            }
        }
        else if (entity is ProxyEntity proxy)
        {
            primitiveType = proxy.function.AsUInt32;
            primitiveName = proxy.function.IsFunctionType ? proxy.function.AsFunctionType.ToString() : $"Proxy_0x{primitiveType:X8}";
        }
        else if (entity is VariableEntity variable)
        {
            primitiveType = (uint)variable.type;
            primitiveName = $"Variable:{variable.type}";
        }
        else if (entity is AliasEntity)
        {
            primitiveName = "Alias";
        }

        var entityName = GetEntityName(entity, primitiveName);
        // Built-in local-effect primitives must not inherit World merely from
        // their containing mission/composite name. Test the effect endpoint,
        // not whether a Player appeared earlier in the causal graph.
        var domain = ClassifyRuntimeDomain(kind, composite.name, primitiveName, entityName);
        var flags = WorldEntityFlags.None;
        if (kind == WorldEntityKind.CompositeInstance) flags |= WorldEntityFlags.CompositeInstance;
        if (entity.childLinks.Count != 0) flags |= WorldEntityFlags.HasOutgoingLinks;
        if (kind == WorldEntityKind.CompositeInstance && entity.parameters.Count != 0) flags |= WorldEntityFlags.HasOverrides;
        if (IsEnvironmentPrimitive(primitiveName)) flags |= WorldEntityFlags.EnvironmentPrimitive;
        if (IsNondeterministicBoundary(primitiveName)) flags |= WorldEntityFlags.NondeterministicBoundary;
        if (IsZonePrimitive(primitiveName)) flags |= WorldEntityFlags.ZonePrimitive;
        if (runtimeBaseInstanceId != 0U) flags |= WorldEntityFlags.RuntimeAddressable;
        if (IsPlayerMissionCompleteEndpoint(entity)) flags |= WorldEntityFlags.PlayerMissionComplete;

        var id = ComputeEntityId(composite.shortGUID.AsUInt32, entity.shortGUID.AsUInt32, kind, path);
        var zoneId = flags.HasFlag(WorldEntityFlags.ZonePrimitive)
            ? ComputeStableZoneId(composite.shortGUID.AsUInt32, entity.shortGUID.AsUInt32, path)
            : 0U;

        return new WorldEntityDescriptor(
            id,
            domain,
            kind,
            flags,
            composite.shortGUID.AsUInt32,
            entity.shortGUID.AsUInt32,
            pathId,
            hierarchyHash,
            entity.shortGUID.AsUInt32,
            runtimeBaseInstanceId == 0U
                ? 0U
                : WorldManifestRuntimeIdentity.ComputeCathodeCompositeInstanceId(path, runtimeBaseInstanceId),
            zoneId,
            primitiveType,
            primitiveName,
            entityName);
    }

    // Inspect the actual function and its arguments, rather than a map name,
    // entity name or ShortGuid. This endpoint is used by the shared Survivor UI
    // before the playlist/results branch, including maps launched outside it.
    internal static bool IsPlayerMissionCompleteEndpoint(Entity entity) =>
        entity is FunctionEntity function
        && function.function == ShortGuidUtils.Generate("FlashInvoke")
        && function.parameters.Any(parameter => parameter.name == ShortGuidUtils.Generate("method")
            && parameter.content is cString { value: "challengemapOpenedResults" })
        && function.parameters.Any(parameter => parameter.name == ShortGuidUtils.Generate("layer_name")
            && parameter.content is cString { value: "ChallengeMapOverlay" });

    private WorldEntityKind GetKind(Commands commands, Entity entity)
    {
        if (entity is FunctionEntity function)
        {
            return FindComposite(commands, function.function) is null ? WorldEntityKind.Function : WorldEntityKind.CompositeInstance;
        }
        if (entity is VariableEntity) return WorldEntityKind.Variable;
        if (entity is ProxyEntity) return WorldEntityKind.Proxy;
        if (entity is AliasEntity) return WorldEntityKind.Alias;
        return WorldEntityKind.Unknown;
    }

    private static string GetEntityName(Entity entity, string primitiveName)
    {
        // Entity names are diagnostic metadata only. Keep this intentionally limited to
        // directly exposed CathodeLib fields so manifest identity never depends on a
        // resolver/helper API whose semantics may change between library versions.
        if (entity is VariableEntity variable) return variable.name.ToString();
        return $"{primitiveName}@{entity.shortGUID}";
    }

    private readonly record struct CompositePublicInputCanonicalKey(
        WorldEntityId OwnerCompositeInstance,
        uint PublicInputShortGuid);

    private readonly record struct CompositeReachability(
        CompositePublicInputCanonicalKey Canonical,
        bool Ambiguous)
    {
        public static CompositeReachability Unique(CompositePublicInputCanonicalKey canonical) =>
            new(canonical, false);

        public static CompositeReachability AmbiguousState() =>
            new(default, true);
    }

    private void BuildCompositePublicInputCanonicalAnchors()
    {
        _compositePublicInputCanonicalAnchors.Clear();
        if (_compositePublicInputs.Count == 0 || _connections.Count == 0) return;

        // BuildFix05 does NOT claim generic input/output transfer semantics for
        // arbitrary primitives. It starts only from direct public bridges whose
        // target input is the STATIC CONFIRMED generic set_false input, then
        // follows COMMANDS graph connectivity conservatively. Only downstream
        // set_false endpoints with one unique public set_false lineage survive.
        // Any competing public set_false lineage marks the endpoint ambiguous and
        // it is omitted from the runtime canonicalization index.
        var adjacency = _connections
            .GroupBy(static value => value.SourceEntity)
            .ToDictionary(static group => group.Key, static group => group.ToArray());

        var entityState = new Dictionary<WorldEntityId, CompositeReachability>();
        var anchorState = new Dictionary<(WorldEntityId Entity, uint Input), CompositeReachability>();
        var queue = new Queue<WorldEntityId>();

        static bool SameCanonical(
            CompositePublicInputCanonicalKey left,
            CompositePublicInputCanonicalKey right) =>
            left.OwnerCompositeInstance == right.OwnerCompositeInstance &&
            left.PublicInputShortGuid == right.PublicInputShortGuid;

        void MergeAnchor(
            WorldEntityId entity,
            uint input,
            CompositeReachability incoming)
        {
            if (!entity.IsValid || input == 0U) return;
            var key = (entity, input);
            if (!anchorState.TryGetValue(key, out var existing))
            {
                anchorState.Add(key, incoming);
                return;
            }
            if (existing.Ambiguous) return;
            if (incoming.Ambiguous || !SameCanonical(existing.Canonical, incoming.Canonical))
                anchorState[key] = CompositeReachability.AmbiguousState();
        }

        void MergeEntity(WorldEntityId entity, CompositeReachability incoming)
        {
            if (!entity.IsValid) return;
            if (!entityState.TryGetValue(entity, out var existing))
            {
                entityState.Add(entity, incoming);
                queue.Enqueue(entity);
                return;
            }
            if (existing.Ambiguous) return;
            if (incoming.Ambiguous || !SameCanonical(existing.Canonical, incoming.Canonical))
            {
                entityState[entity] = CompositeReachability.AmbiguousState();
                queue.Enqueue(entity);
            }
        }

        foreach (var bridge in _compositePublicInputs)
        {
            if (!bridge.NoTypedData ||
                bridge.InternalTargetInputShortGuid != LogicSwitchSetFalseInputShortGuid)
            {
                continue;
            }

            var canonical = new CompositePublicInputCanonicalKey(
                bridge.OwnerCompositeInstance,
                bridge.PublicInputShortGuid);
            var reachability = CompositeReachability.Unique(canonical);
            MergeEntity(bridge.InternalTargetEntity, reachability);
            MergeAnchor(
                bridge.InternalTargetEntity,
                LogicSwitchSetFalseInputShortGuid,
                reachability);
        }

        while (queue.Count != 0)
        {
            var source = queue.Dequeue();
            if (!entityState.TryGetValue(source, out var sourceState) ||
                !adjacency.TryGetValue(source, out var outgoing))
            {
                continue;
            }

            foreach (var connection in outgoing)
            {
                if (connection.TargetParameter == LogicSwitchSetFalseInputShortGuid)
                {
                    MergeAnchor(
                        connection.TargetEntity,
                        connection.TargetParameter,
                        sourceState);
                }
                MergeEntity(connection.TargetEntity, sourceState);
            }
        }

        var allowedPublicInputs = _compositePublicInputs
            .Where(static value => value.NoTypedData)
            .Select(static value => new CompositePublicInputCanonicalKey(
                value.OwnerCompositeInstance, value.PublicInputShortGuid))
            .ToHashSet();

        foreach (var (endpoint, state) in anchorState)
        {
            if (state.Ambiguous ||
                !allowedPublicInputs.Contains(state.Canonical) ||
                endpoint.Input != LogicSwitchSetFalseInputShortGuid)
            {
                continue;
            }

            if (_compositePublicInputCanonicalAnchors.Count >=
                MaxCompositePublicInputCanonicalAnchors)
            {
                throw new InvalidDataException(
                    $"Composite public-input canonical-anchor count exceeded {MaxCompositePublicInputCanonicalAnchors}; refusing an unbounded manifest.");
            }
            _compositePublicInputCanonicalAnchors.Add(
                new WorldCompositePublicInputCanonicalAnchor(
                    state.Canonical.OwnerCompositeInstance,
                    state.Canonical.PublicInputShortGuid,
                    endpoint.Entity,
                    endpoint.Input));
        }

        _warnings.Add(
            $"Fix3.24 BuildFix05 generated {_compositePublicInputCanonicalAnchors.Count} unique payloadless public set_false descendant anchors from the current COMMANDS graph; ambiguous endpoints were omitted fail-closed.");
    }

    private readonly record struct CausalProfileNode(
        WorldEntityId Entity,
        uint Input, bool Output = false);

    private readonly record struct CausalSemanticContract(
        WorldReplicationContractKind Kind,
        WorldEntityId TargetEntity,
        uint Operation)
    {
        internal bool IsKnown =>
            Kind != WorldReplicationContractKind.Unknown &&
            TargetEntity.IsValid &&
            Operation != 0U;
    }

    private void BuildCompositePublicInputCausalProfiles()
    {
        _compositePublicInputCausalProfiles.Clear();
        if (_compositePublicInputs.Count == 0 && _connections.Count == 0) return;

        // WM09 keeps the shared endpoint/relevance graph from WM08, then derives
        // an orthogonal semantic contract for each runtime boundary. The graph
        // is built once; native capture performs no graph traversal.
        const int MaxSharedCausalGraphNodes = 2_000_000;

        var entities = _entities.ToDictionary(static value => value.EntityId);
        var adjacency = _connections
            .GroupBy(static value => value.SourceEntity)
            .ToDictionary(static group => group.Key, static group => group.ToArray());
        var outputEdges = _connections
            .GroupBy(static edge => (edge.SourceEntity, edge.SourceParameter))
            .ToDictionary(static group => group.Key, static group => group.ToArray());
        var publicBridges = _compositePublicInputs
            .GroupBy(static value => (value.OwnerCompositeInstance, value.PublicInputShortGuid))
            .ToDictionary(static group => group.Key, static group => group.ToArray());

        var nodeIndexes = new Dictionary<CausalProfileNode, int>();
        var nodes = new List<CausalProfileNode>();
        var localFlags = new List<WorldCausalDependencyFlags>();
        var successors = new List<List<int>>();
        var predecessors = new List<List<int>>();
        var discovery = new Queue<int>();

        int GetOrAddNode(CausalProfileNode node)
        {
            if (nodeIndexes.TryGetValue(node, out var existing)) return existing;
            if (nodes.Count >= MaxSharedCausalGraphNodes) return -1;

            var index = nodes.Count;
            nodeIndexes.Add(node, index);
            nodes.Add(node);
            localFlags.Add(WorldCausalDependencyFlags.None);
            successors.Add(new List<int>());
            predecessors.Add(new List<int>());
            discovery.Enqueue(index);
            return index;
        }

        foreach (var bridges in publicBridges.Values)
        {
            foreach (var bridge in bridges)
            {
                _ = GetOrAddNode(new CausalProfileNode(
                    bridge.InternalTargetEntity,
                    bridge.InternalTargetInputShortGuid));
            }
        }

        // 008BE0D0 exposes one exact emitted port. Keeping output nodes distinct
        // from consumed inputs is what lets WM09 defer a decision/RNG input to
        // the first concrete branch without replaying the decision itself.
        foreach (var port in outputEdges.Keys)
            _ = GetOrAddNode(new CausalProfileNode(port.SourceEntity, port.SourceParameter, true));

        var sharedGraphTruncated = false;
        while (discovery.Count != 0)
        {
            var index = discovery.Dequeue();
            var node = nodes[index];

            if (!entities.TryGetValue(node.Entity, out var descriptor))
            {
                localFlags[index] |= WorldCausalDependencyFlags.UnknownDependency;
                continue;
            }

            if (!node.Output) localFlags[index] |= descriptor.Domain switch
            {
                WorldDomain.Player => WorldCausalDependencyFlags.PlayerDependency,
                WorldDomain.AI => WorldCausalDependencyFlags.AiDependency,
                WorldDomain.UI => WorldCausalDependencyFlags.UiDependency,
                WorldDomain.AIMP => WorldCausalDependencyFlags.AimpDependency,
                WorldDomain.Unknown => descriptor.Kind is WorldEntityKind.Function or
                    WorldEntityKind.Proxy or WorldEntityKind.Alias
                        ? WorldCausalDependencyFlags.UnknownDependency
                        : WorldCausalDependencyFlags.None,
                _ => WorldCausalDependencyFlags.None
            };

            // WorldEffect remains a DOMAIN/reachability fact in WM09. It is no
            // longer sufficient for network admission.
            if (!node.Output && descriptor.Domain == WorldDomain.World &&
                descriptor.Kind is WorldEntityKind.Function or
                    WorldEntityKind.Proxy or WorldEntityKind.Alias)
            {
                localFlags[index] |= WorldCausalDependencyFlags.WorldEffect;
            }

            void AddSuccessor(CausalProfileNode successor)
            {
                var targetIndex = GetOrAddNode(successor);
                if (targetIndex < 0)
                {
                    localFlags[index] |=
                        WorldCausalDependencyFlags.Truncated |
                        WorldCausalDependencyFlags.UnknownDependency;
                    sharedGraphTruncated = true;
                    return;
                }

                successors[index].Add(targetIndex);
                predecessors[targetIndex].Add(index);
            }

            if (node.Output)
            {
                if (outputEdges.TryGetValue((node.Entity, node.Input), out var actualPort))
                    foreach (var edge in actualPort)
                        AddSuccessor(new CausalProfileNode(edge.TargetEntity, edge.TargetParameter));
                continue;
            }

            if (descriptor.Kind == WorldEntityKind.CompositeInstance)
            {
                if (node.Input != 0U &&
                    publicBridges.TryGetValue((node.Entity, node.Input), out var nested))
                {
                    foreach (var bridge in nested)
                    {
                        AddSuccessor(new CausalProfileNode(
                            bridge.InternalTargetEntity,
                            bridge.InternalTargetInputShortGuid));
                    }
                }
                continue;
            }

            if (!adjacency.TryGetValue(node.Entity, out var outgoing)) continue;
            var possibleOutputs = outgoing.Select(static edge => edge.SourceParameter).Distinct().ToArray();
            if (possibleOutputs.Length > 1)
                localFlags[index] |= WorldCausalDependencyFlags.Branching;
            foreach (var port in possibleOutputs)
                AddSuccessor(new CausalProfileNode(node.Entity, port, true));
        }

        // Dependency flags are monotonic and preserve the WM07 fixed-point
        // contract. They remain orthogonal to relevance.
        var summaries = localFlags.ToArray();
        var propagation = new Queue<int>();
        var queued = new bool[nodes.Count];
        for (var index = 0; index < nodes.Count; index++)
        {
            propagation.Enqueue(index);
            queued[index] = true;
        }

        while (propagation.Count != 0)
        {
            var index = propagation.Dequeue();
            queued[index] = false;

            var merged = localFlags[index];
            foreach (var successor in successors[index])
                merged |= summaries[successor];

            if (merged == summaries[index]) continue;
            summaries[index] = merged;

            foreach (var predecessor in predecessors[index])
            {
                if (queued[predecessor]) continue;
                propagation.Enqueue(predecessor);
                queued[predecessor] = true;
            }
        }

        // Iterative Kosaraju condensation. Recursion is deliberately avoided:
        // ChallengeMap16 has >1M profiles and a recursive SCC walk would make
        // compiler stack depth depend on level topology.
        var visited = new bool[nodes.Count];
        var edgeCursor = new int[nodes.Count];
        var finishOrder = new List<int>(nodes.Count);
        var dfsStack = new List<int>();
        for (var startNode = 0; startNode < nodes.Count; ++startNode)
        {
            if (visited[startNode]) continue;
            visited[startNode] = true;
            dfsStack.Add(startNode);
            while (dfsStack.Count != 0)
            {
                var nodeIndex = dfsStack[^1];
                var cursor = edgeCursor[nodeIndex];
                if (cursor < successors[nodeIndex].Count)
                {
                    var next = successors[nodeIndex][cursor];
                    edgeCursor[nodeIndex] = cursor + 1;
                    if (!visited[next])
                    {
                        visited[next] = true;
                        dfsStack.Add(next);
                    }
                    continue;
                }
                finishOrder.Add(nodeIndex);
                dfsStack.RemoveAt(dfsStack.Count - 1);
            }
        }

        var component = Enumerable.Repeat(-1, nodes.Count).ToArray();
        var componentSizes = new List<int>();
        var reverseStack = new List<int>();
        for (var orderIndex = finishOrder.Count - 1; orderIndex >= 0; --orderIndex)
        {
            var startNode = finishOrder[orderIndex];
            if (component[startNode] >= 0) continue;
            var componentId = componentSizes.Count;
            var size = 0;
            component[startNode] = componentId;
            reverseStack.Add(startNode);
            while (reverseStack.Count != 0)
            {
                var nodeIndex = reverseStack[^1];
                reverseStack.RemoveAt(reverseStack.Count - 1);
                ++size;
                foreach (var previous in predecessors[nodeIndex])
                {
                    if (component[previous] >= 0) continue;
                    component[previous] = componentId;
                    reverseStack.Add(previous);
                }
            }
            componentSizes.Add(size);
        }

        var cyclic = new bool[nodes.Count];
        for (var index = 0; index < nodes.Count; ++index)
        {
            if (componentSizes[component[index]] > 1 || successors[index].Contains(index))
                cyclic[index] = true;
        }

        static WorldReplicationRelevanceEvidence SemanticEvidence(
            WorldReplicationSemanticCatalog.Semantics semantics)
        {
            var evidence = semantics.NativeHandlerCertified
                ? WorldReplicationRelevanceEvidence.NativeHandlerEvidence
                : WorldReplicationRelevanceEvidence.None;
            evidence |= semantics.SemanticClass switch
            {
                WorldReplicationSemanticClass.RoutingOnly or
                    WorldReplicationSemanticClass.CompositeRouting =>
                    WorldReplicationRelevanceEvidence.RoutingOnly,
                WorldReplicationSemanticClass.SchedulerMaintenanceOnly =>
                    WorldReplicationRelevanceEvidence.SchedulerMaintenance,
                WorldReplicationSemanticClass.InternalStateOnly =>
                    WorldReplicationRelevanceEvidence.InternalStateOnly,
                _ => WorldReplicationRelevanceEvidence.None
            };
            if (semantics.ReplayHistorySensitive)
                evidence |= WorldReplicationRelevanceEvidence.ReplayHistorySensitive;
            if (semantics.ExactInputCertified)
                evidence |= WorldReplicationRelevanceEvidence.ExactInputSemantic;
            return evidence;
        }

        static uint Pack(
            WorldReplicationRelevanceDisposition disposition,
            WorldReplicationSemanticClass semanticClass,
            WorldReplicationRelevanceEvidence evidence) =>
            WorldReplicationRelevanceCodec.Pack(disposition, semanticClass, evidence);

        static WorldReplicationRelevanceDisposition Disposition(uint packed) =>
            WorldReplicationRelevanceCodec.Disposition(packed);

        var relevance = new uint[nodes.Count];
        var semanticContracts = new CausalSemanticContract[nodes.Count];

        CausalSemanticContract ResolveUniqueDescendantContract(IEnumerable<int> childIndexes)
        {
            var observed = false;
            var resolved = default(CausalSemanticContract);
            foreach (var childIndex in childIndexes)
            {
                var childPacked = relevance[childIndex];
                var childDisposition = Disposition(childPacked);
                if (childDisposition == WorldReplicationRelevanceDisposition.NotRelevant)
                    continue;

                var childEvidence = WorldReplicationRelevanceCodec.Evidence(childPacked);
                if (childDisposition == WorldReplicationRelevanceDisposition.Unknown &&
                    !childEvidence.HasFlag(WorldReplicationRelevanceEvidence.IntrinsicSharedEffect))
                {
                    return default;
                }

                var childContract = semanticContracts[childIndex];
                if (!childContract.IsKnown)
                    return default;

                if (!observed)
                {
                    resolved = childContract;
                    observed = true;
                    continue;
                }

                if (resolved != childContract)
                    return default;
            }

            return observed ? resolved : default;
        }

        // finishOrder stores successors before predecessors on the SCC DAG. All
        // cyclic SCCs are fail-closed, so a single forward pass computes every
        // acyclic endpoint without per-profile graph traversal.
        foreach (var index in finishOrder)
        {
            var node = nodes[index];
            var summary = summaries[index];
            var semantic = node.Output
                ? new WorldReplicationSemanticCatalog.Semantics(
                    WorldReplicationSemanticCatalog.Effect.None,
                    WorldReplicationSemanticClass.RoutingOnly,
                    WorldReplicationContractKind.Unknown,
                    WorldReplicationSemanticCatalog.Routing.Forwarding,
                    false, true, false, true, false)
                : entities.TryGetValue(node.Entity, out var descriptor)
                    ? WorldReplicationSemanticCatalog.Classify(descriptor, node.Input)
                    : WorldReplicationSemanticCatalog.Unknown();
            var evidence = SemanticEvidence(semantic) |
                WorldReplicationRelevanceEvidence.StructuralGraphEvidence;

            if (!summary.HasFlag(WorldCausalDependencyFlags.WorldEffect))
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.NotRelevant,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.NoReplicableSink);
                continue;
            }

            if (summary.HasFlag(WorldCausalDependencyFlags.Truncated))
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.Unknown,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.SemanticUnknown);
                continue;
            }

            if (semantic.Effect == WorldReplicationSemanticCatalog.Effect.Shared)
            {
                if (semantic.ContractKind != WorldReplicationContractKind.Unknown &&
                    entities.TryGetValue(node.Entity, out var semanticTarget) &&
                    semanticTarget.Flags.HasFlag(WorldEntityFlags.RuntimeAddressable) &&
                    node.Input != 0U)
                {
                    semanticContracts[index] = new CausalSemanticContract(
                        semantic.ContractKind,
                        node.Entity,
                        node.Input);
                }

                var sinkEvidence = evidence |
                    WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                    WorldReplicationRelevanceEvidence.IntrinsicSharedEffect;
                if (semantic.EffectBeforeOutput)
                    sinkEvidence |= WorldReplicationRelevanceEvidence.EffectBeforeOutput;

                // A proven intrinsic mutation is never silently deferred. If
                // replay at this exact boundary is not certified (for example a
                // history-sensitive state machine), mark the boundary Unknown
                // but retain IntrinsicSharedEffect evidence. The immediately
                // preceding exact capturable cause may then become a carrier.
                relevance[index] = Pack(
                    semantic.TakeoverCertified
                        ? WorldReplicationRelevanceDisposition.ReplicateIntrinsic
                        : WorldReplicationRelevanceDisposition.Unknown,
                    semantic.SemanticClass,
                    semantic.TakeoverCertified
                        ? sinkEvidence
                        : sinkEvidence | WorldReplicationRelevanceEvidence.SemanticUnknown);
                continue;
            }

            if (semantic.Effect == WorldReplicationSemanticCatalog.Effect.Unknown)
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.Unknown,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.SemanticUnknown);
                continue;
            }

            // A Door's completion/proximity graph can cycle back to this
            // output. The exact outgoing cut remains finite: every explicit
            // edge must be the same certified force command. Inspect that cut
            // before the general SCC veto, without following later callbacks.
            if (node.Output && successors[index].Count > 0)
            {
                var directEvent = default(CausalSemanticContract);
                var exactCut = true;
                foreach (var childIndex in successors[index])
                {
                    var child = nodes[childIndex];
                    if (child.Output || !entities.TryGetValue(child.Entity, out var childEntity) ||
                        childEntity.PrimitiveType != WorldReplicationSemanticCatalog.DoorPrimitiveType ||
                        !childEntity.Flags.HasFlag(WorldEntityFlags.RuntimeAddressable) ||
                        child.Input is not (WorldReplicationSemanticCatalog.DoorForceOpenInput or
                            WorldReplicationSemanticCatalog.DoorForceCloseInput))
                    {
                        exactCut = false;
                        break;
                    }
                    var contract = new CausalSemanticContract(WorldReplicationContractKind.Event,
                        child.Entity, child.Input);
                    if (directEvent.IsKnown && directEvent != contract) { exactCut = false; break; }
                    directEvent = contract;
                }
                if (exactCut && directEvent.IsKnown)
                {
                    relevance[index] = Pack(WorldReplicationRelevanceDisposition.ReplicateCarrier,
                        WorldReplicationSemanticClass.RoutingOnly,
                        evidence | WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                            WorldReplicationRelevanceEvidence.EmittedEventCarrier);
                    semanticContracts[index] = directEvent;
                    continue;
                }
            }

            if (cyclic[index])
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.Unknown,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.CycleAmbiguous);
                continue;
            }

            var unknownPath = false;
            var relevantDescendant = false;
            var directUnhookedSink = false;
            var unsafeIntrinsicDescendant = false;
            var stateTransitionScheduledSink = false;
            // Successors have already been evaluated in finishOrder. Resolve
            // their semantic meaning once so carrier promotion is allowed only
            // when every relevant path converges on one identical sink.
            var descendantContract = ResolveUniqueDescendantContract(successors[index]);
            foreach (var successor in successors[index])
            {
                var childPacked = relevance[successor];
                var childDisposition = Disposition(childPacked);
                var childEvidence = WorldReplicationRelevanceCodec.Evidence(childPacked);
                if (childDisposition == WorldReplicationRelevanceDisposition.Unknown)
                {
                    // A semantically proven shared mutation whose exact replay
                    // boundary is history-sensitive is different from a wholly
                    // unknown descendant. It demands a carrier before the
                    // mutation; it must not poison that preceding exact cut.
                    if (childEvidence.HasFlag(
                        WorldReplicationRelevanceEvidence.IntrinsicSharedEffect))
                    {
                        unsafeIntrinsicDescendant = true;
                        relevantDescendant = true;
                        continue;
                    }
                    unknownPath = true;
                    continue;
                }
                if (childDisposition == WorldReplicationRelevanceDisposition.NotRelevant)
                    continue;

                relevantDescendant = true;
                if (!node.Output)
                {
                    // Every normal primitive input reaches its explicit outgoing
                    // graph only through 008BE0D0, which is itself captured.
                    continue;
                }

                var targetNode = nodes[successor];
                var targetIsScheduledBoundary =
                    targetNode.Entity != node.Entity &&
                    entities.TryGetValue(targetNode.Entity, out var targetDescriptor) &&
                    targetDescriptor.Kind != WorldEntityKind.CompositeInstance &&
                    (targetDescriptor.Flags & WorldEntityFlags.RuntimeAddressable) != 0;

                // Patch 4: an exact StateTransition consumer is semantically
                // convergent but its transition-only outputs are not. Replaying
                // the ScheduledInput itself was proven unsafe by the Patch 2/3
                // in-game diagnostics. The immediately preceding captured
                // EmittedOutput is therefore the preferred high-level cause,
                // but only when the descendant contract is exact and unique.
                var childContract = semanticContracts[successor];
                if (targetIsScheduledBoundary &&
                    childDisposition == WorldReplicationRelevanceDisposition.ReplicateIntrinsic &&
                    childContract.IsKnown &&
                    childContract.Kind == WorldReplicationContractKind.StateTransition)
                {
                    stateTransitionScheduledSink = true;
                }

                if (!targetIsScheduledBoundary &&
                    (childDisposition is WorldReplicationRelevanceDisposition.ReplicateIntrinsic or
                        WorldReplicationRelevanceDisposition.ReplicateCarrier))
                {
                    // AI.exe.c FUN_008BE0D0 proves that same-entity explicit
                    // edges dispatch +6C -> +394 directly and therefore bypass
                    // 00926680. The output is the last captured boundary before
                    // this known sink and must carry it.
                    directUnhookedSink = true;
                }
            }

            if (unknownPath)
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.Unknown,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.SemanticUnknown);
            }
            else if (node.Output && unsafeIntrinsicDescendant)
            {
                // Exact EmittedOutput is the first safe cause before a proven
                // shared effect whose own replay boundary is not certified.
                // Replaying the output reproduces the mutation through retail
                // without taking over the history-sensitive consumer itself.
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.ReplicateCarrier,
                    WorldReplicationSemanticClass.RoutingOnly,
                    evidence |
                        WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                        WorldReplicationRelevanceEvidence.ReplayHistorySensitive);
            }
            else if (node.Output && stateTransitionScheduledSink &&
                descendantContract.IsKnown &&
                descendantContract.Kind == WorldReplicationContractKind.StateTransition)
            {
                // Patch 4 high-level apply: stop one captured CATHODE boundary
                // before the exact ScheduledInput StateTransition consumer.
                // ResolveUniqueDescendantContract above proves that every
                // relevant successor agrees on the same semantic target/op.
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.ReplicateCarrier,
                    WorldReplicationSemanticClass.RoutingOnly,
                    evidence |
                        WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                        WorldReplicationRelevanceEvidence.StateTransitionCarrier);
            }
            else if (!relevantDescendant)
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.NotRelevant,
                    semantic.SemanticClass,
                    evidence | WorldReplicationRelevanceEvidence.NoReplicableSink);
            }
            else if (node.Output && directUnhookedSink)
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.ReplicateCarrier,
                    WorldReplicationSemanticClass.RoutingOnly,
                    evidence |
                        WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                        WorldReplicationRelevanceEvidence.DirectUnhookedSink);
            }
            else
            {
                relevance[index] = Pack(
                    WorldReplicationRelevanceDisposition.DeferToDescendant,
                    semantic.SemanticClass,
                    evidence |
                        WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                        WorldReplicationRelevanceEvidence.CompleteDescendantFrontier);
            }

            semanticContracts[index] = descendantContract;
        }

        // Patch 5.1 relaxed semantic frontier. The primary WM09 contract array
        // intentionally stops at a semantically Unknown primitive. That was
        // useful for lower-boundary takeover but too restrictive for choosing a
        // high-level Composite cause: real CATHODE graphs frequently contain an
        // unknown routing/helper primitive between a public input and a fully
        // known StateTransition sink.
        //
        // Compute the relaxed view on the SCC condensation graph, not directly
        // on individual nodes. That means cyclic CATHODE routing can remain a
        // valid high-level cause when every known contract in/after the cycle
        // agrees. We never invent a contract: only already-known contracts are
        // propagated, and any known disagreement marks the whole SCC lineage as
        // conflicting.
        var componentCount = componentSizes.Count;
        var componentHead = Enumerable.Repeat(-1, componentCount).ToArray();
        var componentNext = Enumerable.Repeat(-1, nodes.Count).ToArray();
        for (var index = 0; index < nodes.Count; ++index)
        {
            var componentId = component[index];
            componentNext[index] = componentHead[componentId];
            componentHead[componentId] = index;
        }

        var componentOrder = new List<int>(componentCount);
        var componentQueued = new bool[componentCount];
        foreach (var nodeIndex in finishOrder)
        {
            var componentId = component[nodeIndex];
            if (componentQueued[componentId])
                continue;
            componentQueued[componentId] = true;
            componentOrder.Add(componentId);
        }

        var relaxedComponentContracts = new CausalSemanticContract[componentCount];
        var relaxedComponentConflicts = new bool[componentCount];
        foreach (var componentId in componentOrder)
        {
            var observed = false;
            var resolved = default(CausalSemanticContract);
            var conflict = false;

            void MergeKnownContract(CausalSemanticContract candidate)
            {
                if (!candidate.IsKnown || conflict)
                    return;
                if (!observed)
                {
                    resolved = candidate;
                    observed = true;
                    return;
                }
                if (resolved != candidate)
                    conflict = true;
            }

            for (var nodeIndex = componentHead[componentId];
                 nodeIndex >= 0;
                 nodeIndex = componentNext[nodeIndex])
            {
                if (Disposition(relevance[nodeIndex]) ==
                    WorldReplicationRelevanceDisposition.NotRelevant)
                    continue;

                MergeKnownContract(semanticContracts[nodeIndex]);

                foreach (var successor in successors[nodeIndex])
                {
                    if (Disposition(relevance[successor]) ==
                        WorldReplicationRelevanceDisposition.NotRelevant)
                        continue;
                    var successorComponent = component[successor];
                    if (successorComponent == componentId)
                        continue;
                    if (relaxedComponentConflicts[successorComponent])
                    {
                        conflict = true;
                        continue;
                    }
                    MergeKnownContract(relaxedComponentContracts[successorComponent]);
                }
            }

            relaxedComponentConflicts[componentId] = conflict;
            if (!conflict && observed)
                relaxedComponentContracts[componentId] = resolved;
        }

        var relaxedSemanticContracts = new CausalSemanticContract[nodes.Count];
        var relaxedSemanticContractConflicts = new bool[nodes.Count];
        for (var index = 0; index < nodes.Count; ++index)
        {
            if (Disposition(relevance[index]) == WorldReplicationRelevanceDisposition.NotRelevant)
                continue;
            var componentId = component[index];
            relaxedSemanticContracts[index] = relaxedComponentContracts[componentId];
            relaxedSemanticContractConflicts[index] = relaxedComponentConflicts[componentId];
        }

        (uint Relevance, CausalSemanticContract Contract) CompositeEntryProfile(
            IEnumerable<WorldCompositePublicInputDescriptor> bridges,
            WorldCausalDependencyFlags aggregateFlags)
        {
            // Patch 5.1 deliberately separates a genuinely incomplete graph
            // from a merely imperfectly-classified descendant. Patch 5 treated
            // every UnknownDependency anywhere below the CompositeInput as a
            // veto. In real CATHODE graphs that is too strict: local/routing
            // side branches often remain semantically unknown even though all
            // known shared-world descendants converge on one exact transition.
            //
            // We now promote when every *known relevant* contract agrees on one
            // StateTransition. Unknown-but-non-truncated relevant paths are
            // tolerated and marked with CompositeRelevantFrontierRelaxed so the
            // runtime can distinguish this deliberately wider admission. A
            // missing graph node/truncation or a conflicting known semantic
            // contract still prevents high-level promotion. A cycle alone is
            // no longer a veto because retail CATHODE already owns cycle
            // execution; it simply marks the frontier as relaxed.
            var hardUnknownPath = false;
            var relaxedUnknownRelevantPath = false;
            var relevantDescendant = false;
            var directSink = false;
            var historySensitive = false;
            var cycleAmbiguous = false;
            var knownContractConflict = false;
            var observedKnownContract = false;
            var relaxedContract = default(CausalSemanticContract);

            foreach (var bridge in bridges)
            {
                var targetNode = new CausalProfileNode(
                    bridge.InternalTargetEntity,
                    bridge.InternalTargetInputShortGuid);
                if (!nodeIndexes.TryGetValue(targetNode, out var targetIndex))
                {
                    // Public bridge targets are seeded before graph expansion;
                    // failure here therefore means the graph itself could not
                    // represent the entry (normally capacity/truncation). Keep
                    // this as a real hard stop rather than guessing.
                    hardUnknownPath = true;
                    continue;
                }

                var childPacked = relevance[targetIndex];
                var childDisposition = Disposition(childPacked);
                var childEvidence = WorldReplicationRelevanceCodec.Evidence(childPacked);
                var childSummary = summaries[targetIndex];
                var childContract = relaxedSemanticContracts[targetIndex];
                knownContractConflict |= relaxedSemanticContractConflicts[targetIndex];

                historySensitive |= childEvidence.HasFlag(
                    WorldReplicationRelevanceEvidence.ReplayHistorySensitive);
                cycleAmbiguous |= childEvidence.HasFlag(
                    WorldReplicationRelevanceEvidence.CycleAmbiguous);

                if (childDisposition == WorldReplicationRelevanceDisposition.NotRelevant)
                    continue;

                var childRelevant = false;
                switch (childDisposition)
                {
                    case WorldReplicationRelevanceDisposition.Unknown:
                        if (childEvidence.HasFlag(
                            WorldReplicationRelevanceEvidence.IntrinsicSharedEffect))
                        {
                            // A history-sensitive but semantically identified
                            // shared mutation is still a relevant descendant and
                            // the public Composite may be its higher carrier.
                            childRelevant = true;
                            directSink = true;
                        }
                        else if (childSummary.HasFlag(WorldCausalDependencyFlags.WorldEffect))
                        {
                            // Patch 5.1: do not let a non-truncated Unknown
                            // side branch poison an otherwise converged gameplay
                            // cause. We retain the aggregate UnknownDependency
                            // flag for diagnostics and mark the carrier as using
                            // the relaxed relevant-frontier rule.
                            childRelevant = true;
                            relaxedUnknownRelevantPath = true;
                        }
                        break;
                    case WorldReplicationRelevanceDisposition.DeferToDescendant:
                        childRelevant = true;
                        break;
                    case WorldReplicationRelevanceDisposition.ReplicateIntrinsic:
                    case WorldReplicationRelevanceDisposition.ReplicateCarrier:
                        // Composite public-input bridges are internal graph entry
                        // edges; no 00926680 interception is assumed between the
                        // public port and its child target.
                        childRelevant = true;
                        directSink = true;
                        break;
                }

                if (!childRelevant)
                    continue;

                relevantDescendant = true;

                // Convergence is now based on known relevant contracts only.
                // Unknown contracts are tolerated (and diagnosed above), but a
                // known disagreement remains a hard semantic conflict.
                if (!childContract.IsKnown)
                {
                    relaxedUnknownRelevantPath = true;
                    continue;
                }

                if (!observedKnownContract)
                {
                    relaxedContract = childContract;
                    observedKnownContract = true;
                }
                else if (relaxedContract != childContract)
                {
                    knownContractConflict = true;
                }
            }

            var entryEvidence = WorldReplicationRelevanceEvidence.RoutingOnly |
                WorldReplicationRelevanceEvidence.StructuralGraphEvidence;
            if (historySensitive)
                entryEvidence |= WorldReplicationRelevanceEvidence.ReplayHistorySensitive;
            if (cycleAmbiguous)
                entryEvidence |= WorldReplicationRelevanceEvidence.CycleAmbiguous;

            if (hardUnknownPath)
                return (Pack(WorldReplicationRelevanceDisposition.Unknown,
                        WorldReplicationSemanticClass.CompositeRouting,
                        entryEvidence | WorldReplicationRelevanceEvidence.SemanticUnknown),
                    default);
            if (!relevantDescendant)
                return (Pack(WorldReplicationRelevanceDisposition.NotRelevant,
                        WorldReplicationSemanticClass.CompositeRouting,
                        entryEvidence | WorldReplicationRelevanceEvidence.NoReplicableSink),
                    default);

            var entryContract = !knownContractConflict && observedKnownContract
                ? relaxedContract
                : default;
            var graphTruncated = aggregateFlags.HasFlag(WorldCausalDependencyFlags.Truncated);
            var relaxedFrontier = relaxedUnknownRelevantPath || cycleAmbiguous ||
                aggregateFlags.HasFlag(WorldCausalDependencyFlags.UnknownDependency);

            if (!graphTruncated && !knownContractConflict &&
                entryContract.IsKnown &&
                entryContract.Kind == WorldReplicationContractKind.StateTransition)
            {
                // Patch 5.1: open the high-level carrier boundary to real-world
                // CATHODE graphs. UnknownDependency is no longer a blanket veto.
                // All known relevant descendants must still converge on one
                // exact StateTransition target/operation. Unknown side paths
                // and cycles mark the wider frontier but do not veto it; only
                // truncation and known semantic disagreement remain rejected.
                var carrierEvidence = entryEvidence |
                    WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                    WorldReplicationRelevanceEvidence.CompleteDescendantFrontier |
                    WorldReplicationRelevanceEvidence.CompositeStateTransitionCarrier;
                if (relaxedFrontier)
                    carrierEvidence |= WorldReplicationRelevanceEvidence.CompositeRelevantFrontierRelaxed;

                return (Pack(WorldReplicationRelevanceDisposition.ReplicateCarrier,
                        WorldReplicationSemanticClass.CompositeRouting,
                        carrierEvidence),
                    entryContract);
            }

            if (directSink)
                return (Pack(WorldReplicationRelevanceDisposition.ReplicateCarrier,
                        WorldReplicationSemanticClass.CompositeRouting,
                        entryEvidence |
                            WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                            WorldReplicationRelevanceEvidence.DirectUnhookedSink),
                    entryContract);
            return (Pack(WorldReplicationRelevanceDisposition.DeferToDescendant,
                    WorldReplicationSemanticClass.CompositeRouting,
                    entryEvidence |
                        WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
                        WorldReplicationRelevanceEvidence.CompleteDescendantFrontier),
                entryContract);
        }

        foreach (var entry in publicBridges)
        {
            var flags = WorldCausalDependencyFlags.None;
            foreach (var bridge in entry.Value)
            {
                var node = new CausalProfileNode(
                    bridge.InternalTargetEntity,
                    bridge.InternalTargetInputShortGuid);
                if (!nodeIndexes.TryGetValue(node, out var index))
                {
                    flags |= WorldCausalDependencyFlags.UnknownDependency |
                             WorldCausalDependencyFlags.Truncated;
                    continue;
                }
                flags |= summaries[index];
            }

            var entryProfile = CompositeEntryProfile(entry.Value, flags);

            _compositePublicInputCausalProfiles.Add(
                new WorldCompositePublicInputCausalProfile(
                    entry.Key.OwnerCompositeInstance,
                    entry.Key.PublicInputShortGuid,
                    flags,
                    1U,
                    entryProfile.Relevance,
                    entryProfile.Contract.Kind,
                    entryProfile.Contract.TargetEntity,
                    entryProfile.Contract.Operation));
        }

        foreach (var port in outputEdges.Keys)
        {
            if (!entities.TryGetValue(port.SourceEntity, out var source) ||
                (source.Flags & WorldEntityFlags.RuntimeAddressable) == 0) continue;
            var node = new CausalProfileNode(port.SourceEntity, port.SourceParameter, true);
            var flags = nodeIndexes.TryGetValue(node, out var index)
                ? summaries[index]
                : WorldCausalDependencyFlags.UnknownDependency | WorldCausalDependencyFlags.Truncated;
            var packed = nodeIndexes.TryGetValue(node, out index)
                ? relevance[index]
                : Pack(WorldReplicationRelevanceDisposition.Unknown,
                    WorldReplicationSemanticClass.Unknown,
                    WorldReplicationRelevanceEvidence.SemanticUnknown);
            var contract = nodeIndexes.TryGetValue(node, out index)
                ? semanticContracts[index]
                : default;
            _compositePublicInputCausalProfiles.Add(
                new(port.SourceEntity, port.SourceParameter, flags, 2U, packed,
                    contract.Kind, contract.TargetEntity, contract.Operation));
        }

        // 00926680 selected-consumer profiles. Unknown / NotRelevant / Defer
        // remain retail-local at runtime; only Replicate* may authorize return 2.
        for (var index = 0; index < nodes.Count; ++index)
        {
            var node = nodes[index];
            if (node.Output || !entities.TryGetValue(node.Entity, out var target) ||
                target.Kind == WorldEntityKind.CompositeInstance ||
                (target.Flags & WorldEntityFlags.RuntimeAddressable) == 0) continue;
            var contract = semanticContracts[index];
            _compositePublicInputCausalProfiles.Add(
                new(node.Entity, node.Input, summaries[index], 3U, relevance[index],
                    contract.Kind, contract.TargetEntity, contract.Operation));
        }

        var worldEffect = _compositePublicInputCausalProfiles.Count(static value =>
            value.DependencyFlags.HasFlag(WorldCausalDependencyFlags.WorldEffect));
        var relevanceUnknown = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Disposition(value.ReplicationRelevance) ==
                WorldReplicationRelevanceDisposition.Unknown);
        var relevanceNotRelevant = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Disposition(value.ReplicationRelevance) ==
                WorldReplicationRelevanceDisposition.NotRelevant);
        var relevanceDefer = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Disposition(value.ReplicationRelevance) ==
                WorldReplicationRelevanceDisposition.DeferToDescendant);
        var relevanceIntrinsic = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Disposition(value.ReplicationRelevance) ==
                WorldReplicationRelevanceDisposition.ReplicateIntrinsic);
        var relevanceCarrier = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Disposition(value.ReplicationRelevance) ==
                WorldReplicationRelevanceDisposition.ReplicateCarrier);
        var contractEvent = _compositePublicInputCausalProfiles.Count(static value =>
            value.ReplicationContractKind == WorldReplicationContractKind.Event);
        var contractState = _compositePublicInputCausalProfiles.Count(static value =>
            value.ReplicationContractKind == WorldReplicationContractKind.State);
        var contractTransition = _compositePublicInputCausalProfiles.Count(static value =>
            value.ReplicationContractKind == WorldReplicationContractKind.StateTransition);
        var contractUnknown = _compositePublicInputCausalProfiles.Count -
            contractEvent - contractState - contractTransition;
        var emittedStateTransitionCarriers = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Evidence(value.ReplicationRelevance).HasFlag(
                WorldReplicationRelevanceEvidence.StateTransitionCarrier));
        var compositeStateTransitionCarriers = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Evidence(value.ReplicationRelevance).HasFlag(
                WorldReplicationRelevanceEvidence.CompositeStateTransitionCarrier));
        var relaxedCompositeStateTransitionCarriers = _compositePublicInputCausalProfiles.Count(static value =>
            WorldReplicationRelevanceCodec.Evidence(value.ReplicationRelevance).HasFlag(
                WorldReplicationRelevanceEvidence.CompositeRelevantFrontierRelaxed));
        var strictCompositeStateTransitionCarriers =
            compositeStateTransitionCarriers - relaxedCompositeStateTransitionCarriers;
        var stateTransitionCarriers = emittedStateTransitionCarriers + compositeStateTransitionCarriers;
        var cyclicEndpoints = cyclic.Count(static value => value);

        _warnings.Add(
            $"WS3 WM09 compiler 0x{WorldManifestConstants.CompilerVersion:X8} generated {_compositePublicInputCausalProfiles.Count} causal profiles from one shared COMMANDS graph ({nodes.Count} endpoints, {componentSizes.Count} SCCs, cyclicEndpoints={cyclicEndpoints}): worldEffect={worldEffect}, relevanceUnknown={relevanceUnknown}, notRelevant={relevanceNotRelevant}, defer={relevanceDefer}, replicateIntrinsic={relevanceIntrinsic}, replicateCarrier={relevanceCarrier}, stateTransitionCarrier={stateTransitionCarriers}, emittedStateTransitionCarrier={emittedStateTransitionCarriers}, compositeStateTransitionCarrier={compositeStateTransitionCarriers}, strictCompositeStateTransitionCarrier={strictCompositeStateTransitionCarriers}, relaxedCompositeStateTransitionCarrier={relaxedCompositeStateTransitionCarriers}, contractUnknown={contractUnknown}, contractEvent={contractEvent}, contractState={contractState}, contractStateTransition={contractTransition}, graphTruncated={sharedGraphTruncated}. Patch 5.1 promotes a public CompositeInput when all known relevant descendants converge on one exact StateTransition target/operation; non-truncated UnknownDependency side paths and cycles no longer veto the carrier and are marked relaxed, while truncation and known contract conflicts remain excluded. Patch 4 EmittedOutput carriers remain lower fallbacks.");
    }

    private void ResetCompilationState()
    {
        _warnings.Clear();
        _pathIds.Clear();
        _paths.Clear();
        _entities.Clear();
        _compositePublicInputs.Clear();
        _compositePublicInputCanonicalAnchors.Clear();
        _compositePublicInputCausalProfiles.Clear();
        _connections.Clear();
        _overrides.Clear();
        _references.Clear();
        _zones.Clear();
        _instantiatedCompositeIds.Clear();
        _compositeLookupSource = null;
        _compositeLookup = new();
        _mapContentId = default;
        _excludedAimpCompositeCount = 0;
        _excludedAimpEntityCount = 0;
    }

    // CathodeLib's GetComposite(ShortGuid) scans every composite, and the
    // traversal asks several times per entity. Same first match, indexed once.
    private Composite? FindComposite(Commands commands, ShortGuid id)
    {
        if (!ReferenceEquals(commands, _compositeLookupSource))
        {
            _compositeLookup = new Dictionary<uint, Composite>();
            foreach (var composite in commands.Entries)
                if (composite is not null) _compositeLookup.TryAdd(composite.shortGUID.AsUInt32, composite);
            _compositeLookupSource = commands;
        }
        return _compositeLookup.GetValueOrDefault(id.AsUInt32);
    }

    private uint InternPath(IReadOnlyList<uint> steps, uint manifestNamespaceId, uint runtimeBaseInstanceId)
    {
        var key = $"{manifestNamespaceId:X8}|{string.Join(':', steps.Select(static value => value.ToString("X8")))}";
        if (_pathIds.TryGetValue(key, out var existing)) return existing;

        var pathId = checked((uint)_paths.Count + 1U);
        var hash = ComputeHierarchyHash(steps);
        _pathIds.Add(key, pathId);
        var cathodeHierarchyId = runtimeBaseInstanceId == 0U
            ? 0U
            : WorldManifestRuntimeIdentity.ComputeCathodeCompositeInstanceId(steps, runtimeBaseInstanceId);
        _paths.Add(new WorldInstancePathDescriptor(pathId, steps.ToArray(), hash, cathodeHierarchyId));
        return pathId;
    }

    private WorldManifest? TryLoadCurrentCache(
        string path,
        string pakHash,
        string binHash,
        bool pakPresent,
        bool binPresent)
    {
        try
        {
            var manifest = WorldManifestBinaryCodec.Read(path);
            if (manifest.MapContentId != _mapContentId) return null;
            if (manifest.Source.CommandsPakPresent != pakPresent || manifest.Source.CommandsBinPresent != binPresent) return null;
            if (!string.Equals(manifest.Source.CommandsPakSha256, pakHash, StringComparison.OrdinalIgnoreCase)) return null;
            if (!string.Equals(manifest.Source.CommandsBinSha256, binHash, StringComparison.OrdinalIgnoreCase)) return null;

            // AIMPWM09 intentionally has no cache migration path. Semantic
            // replication contracts, relevance and causal boundary profiles are
            // derived from the complete COMMANDS graph and cannot be reconstructed
            // faithfully from WM08 or any older fixed-width cache.
            return manifest.CompilerVersion == WorldManifestConstants.CompilerVersion
                ? manifest
                : null;
        }
        catch (Exception exception) when (exception is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static Commands LoadCommands(string path)
    {
        // CathodeLib's COMMANDS parsers resolve resource-reference indexes through
        // EnvironmentAnimations / CollisionMaps / RenderableElements while reading.
        // The manifest compiler needs only the CATHODE graph and must also work when
        // those optional level files are unavailable (as in the supplied audit map).
        // Empty, non-loaded parser objects safely turn those resource references into
        // null/empty metadata without changing entity, link, parameter or hierarchy data.
        var placeholderRoot = Path.Combine(Path.GetTempPath(), "AIMP.WorldManifestCompiler", Guid.NewGuid().ToString("N"));
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
        {
            throw new InvalidDataException($"CathodeLib loaded zero composites from '{path}'.");
        }

        // Commands has already materialized the graph. Drop cross-file references so
        // the short-lived placeholder parsers are never retained by the manifest tool.
        commands.ClearReferences();
        return commands;
    }

    private static string ComputeStructuralFingerprint(Commands commands)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);

        var entryPoints = commands.EntryPoints ?? Array.Empty<Composite>();
        writer.Write(entryPoints.Length);
        foreach (var entry in entryPoints) writer.Write(entry?.shortGUID.AsUInt32 ?? 0U);

        var composites = commands.Entries
            .Where(static value => value is not null)
            .OrderBy(static value => value.shortGUID.AsUInt32)
            .ToArray();
        writer.Write(composites.Length);

        foreach (var composite in composites)
        {
            writer.Write(composite.shortGUID.AsUInt32);
            var entities = composite.GetEntities().OrderBy(static value => value.shortGUID.AsUInt32).ToArray();
            writer.Write(entities.Length);

            foreach (var entity in entities)
            {
                writer.Write(entity.shortGUID.AsUInt32);
                writer.Write((uint)entity.variant);

                switch (entity)
                {
                    case FunctionEntity function:
                        writer.Write(function.function.AsUInt32);
                        break;
                    case VariableEntity variable:
                        writer.Write(variable.name.AsUInt32);
                        writer.Write((uint)variable.type);
                        break;
                    case ProxyEntity proxy:
                        writer.Write(proxy.function.AsUInt32);
                        WriteEntityPath(writer, proxy.proxy);
                        break;
                    case AliasEntity alias:
                        WriteEntityPath(writer, alias.alias);
                        break;
                }

                var parameters = entity.parameters
                    .OrderBy(static value => value.name.AsUInt32)
                    .ThenBy(static value => value.content is null ? 0U : (uint)value.content.dataType)
                    .ThenBy(static value => (uint)value.variant)
                    .ToArray();
                writer.Write(parameters.Length);
                foreach (var parameter in parameters)
                {
                    writer.Write(parameter.name.AsUInt32);
                    writer.Write(parameter.content is null ? 0U : (uint)parameter.content.dataType);
                    writer.Write((uint)parameter.variant);
                }

                var links = entity.childLinks
                    .OrderBy(static value => value.ID.AsUInt32)
                    .ThenBy(static value => value.thisParamID.AsUInt32)
                    .ThenBy(static value => value.linkedEntityID.AsUInt32)
                    .ThenBy(static value => value.linkedParamID.AsUInt32)
                    .ToArray();
                writer.Write(links.Length);
                foreach (var link in links)
                {
                    writer.Write(link.ID.AsUInt32);
                    writer.Write(link.thisParamID.AsUInt32);
                    writer.Write(link.linkedEntityID.AsUInt32);
                    writer.Write(link.linkedParamID.AsUInt32);
                }
            }
        }

        writer.Flush();
        return Convert.ToHexString(SHA256.HashData(stream.ToArray())).ToLowerInvariant();
    }

    private static void WriteEntityPath(BinaryWriter writer, EntityPath path)
    {
        var steps = GetPathSteps(path);
        writer.Write(steps.Length);
        foreach (var step in steps) writer.Write(step);
    }

    private static string StablePathKey(IReadOnlyList<uint> steps) =>
        string.Join(":", steps.Select(static value => value.ToString("X8")));

    private static uint[] GetPathSteps(EntityPath path) =>
        (path.path ?? Array.Empty<ShortGuid>()).Select(static value => value.AsUInt32).ToArray();

    internal static WorldEntityId ComputeStableMapContentId(string worldDirectory, bool pakPresent, bool binPresent)
    {
        var trimmed = worldDirectory.TrimEnd('\\', '/');
        var name = Path.GetFileName(trimmed);
        if (name.Equals("WORLD", StringComparison.OrdinalIgnoreCase))
            name = Path.GetFileName(Path.GetDirectoryName(trimmed) ?? string.Empty);
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write("AIMP-WORLDMANIFEST-MAP-V3-LEVEL");
        writer.Write(WorldManifestConstants.FormatVersion);
        writer.Write(pakPresent);
        writer.Write(binPresent);
        writer.Write(name.ToUpperInvariant());
        writer.Flush();
        return Hash128(stream.ToArray());
    }

    internal static WorldEntityId ComputeMapContentId(string pakHash, string binHash, bool pakPresent, bool binPresent)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(MapHashTag);
        writer.Write(WorldManifestConstants.FormatVersion);
        writer.Write(pakPresent);
        writer.Write(binPresent);
        // Source digests are hexadecimal values. Preserve the compiler's historical
        // lowercase encoding when callers supply uppercase SHA256 strings.
        writer.Write(pakHash.ToLowerInvariant());
        writer.Write(binHash.ToLowerInvariant());
        writer.Flush();
        return Hash128(stream.ToArray());
    }

    private WorldEntityId ComputeEntityId(uint compositeGuid, uint entityGuid, WorldEntityKind kind, IReadOnlyList<uint> path)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(EntityHashTag);
        writer.Write(_mapContentId.High);
        writer.Write(_mapContentId.Low);
        writer.Write(compositeGuid);
        writer.Write(entityGuid);
        writer.Write((byte)kind);
        writer.Write(path.Count);
        foreach (var step in path) writer.Write(step);
        writer.Flush();
        return Hash128(stream.ToArray());
    }


    // Runtime hierarchy identity is implemented in AIMP.Protocol so the
    // compiler, binary-format tests and future tooling share the exact retail
    // FUN_008BF3D0-compatible algorithm.

    private static ulong ComputeHierarchyHash(IReadOnlyList<uint> path)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(HierarchyHashTag);
        writer.Write(path.Count);
        foreach (var step in path) writer.Write(step);
        writer.Flush();
        var hash = SHA256.HashData(stream.ToArray());
        return BinaryPrimitives.ReadUInt64LittleEndian(hash);
    }

    private uint ComputeStableZoneId(uint compositeGuid, uint entityGuid, IReadOnlyList<uint> path)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(ZoneHashTag);
        writer.Write(_mapContentId.High);
        writer.Write(_mapContentId.Low);
        writer.Write(compositeGuid);
        writer.Write(entityGuid);
        writer.Write(path.Count);
        foreach (var step in path) writer.Write(step);
        writer.Flush();
        var hash = SHA256.HashData(stream.ToArray());
        var value = BinaryPrimitives.ReadUInt32LittleEndian(hash);
        return value == 0U ? 1U : value;
    }

    private static WorldEntityId Hash128(byte[] data)
    {
        var hash = SHA256.HashData(data);
        return new WorldEntityId(BinaryPrimitives.ReadUInt64LittleEndian(hash), BinaryPrimitives.ReadUInt64LittleEndian(hash.AsSpan(8)));
    }

    private static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static bool IsAimpCompositeName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return false;
        var normalized = name.Replace('/', '\\').TrimStart('\\');
        return normalized.Equals("AIMP", StringComparison.OrdinalIgnoreCase) ||
               normalized.StartsWith("AIMP\\", StringComparison.OrdinalIgnoreCase);
    }

    private static WorldDomain ClassifyCompositeDomain(string? name)
    {
        if (IsAimpCompositeName(name)) return WorldDomain.AIMP;
        if (string.IsNullOrWhiteSpace(name)) return WorldDomain.Unknown;
        var normalized = name.Replace('/', '\\');
        if (ContainsAny(normalized, "UI\\", "PAUSEMENU", "Frontend", "HUD")) return WorldDomain.UI;
        if (ContainsAny(normalized, "AI\\", "NPC\\", "Behaviour", "Behavior")) return WorldDomain.AI;
        if (ContainsAny(normalized, "Player\\", "Inventory\\")) return WorldDomain.Player;
        if (ContainsAny(normalized,
                "World", "Mission", "Environment", "Archetype", "Door", "Lift", "Elevator",
                "Power", "Rewire", "Lighting", "Sound", "Trigger", "Logic", "Sequence", "Level"))
        {
            return WorldDomain.World;
        }
        return WorldDomain.Unknown;
    }

    private static WorldDomain ClassifyDomain(string? compositeName, string primitiveName, string entityName)
    {
        if (IsAimpCompositeName(compositeName)) return WorldDomain.AIMP;

        var value = $"{primitiveName}|{entityName}|{compositeName}";
        if (ContainsAny(value,
                "Flash", "UI_", "MoviePlayer", "PopupMessage", "TutorialMessage", "Objective", "Leaderboard", "TerminalContent"))
        {
            return WorldDomain.UI;
        }

        if (ContainsAny(value,
                "NPC_", "NPC", "CharacterCommand", "Behaviour", "Behavior", "AlienConfig", "Squad", "SenseLimiter", "AI_Notifier"))
        {
            return WorldDomain.AI;
        }

        if (!primitiveName.StartsWith("Trigger", StringComparison.OrdinalIgnoreCase) &&
            ContainsAny(value,
                "THE_PLAYER", "ThePlayer", "PlayerWeapon", "PlayerHas", "PlayerInventory", "DespawnPlayer", "PlayerCampaign"))
        {
            return WorldDomain.Player;
        }

        if (IsEnvironmentPrimitive(primitiveName) || ClassifyCompositeDomain(compositeName) == WorldDomain.World)
        {
            return WorldDomain.World;
        }

        return WorldDomain.Unknown;
    }

    internal static WorldDomain ClassifyRuntimeDomain(
        WorldEntityKind kind, string? compositeName, string primitiveName, string entityName) =>
        (kind is WorldEntityKind.Function or WorldEntityKind.Proxy) && IsPlayerLocalPrimitive(primitiveName)
            ? WorldDomain.Player : ClassifyDomain(compositeName, primitiveName, entityName);

    private static bool IsPlayerLocalPrimitive(string name) =>
        name.StartsWith("Camera", StringComparison.Ordinal) ||
        name.EndsWith("Camera", StringComparison.Ordinal) ||
        name is "Interaction" or "MonitorPadInput" or "MonitorActionMap" or "ButtonMashPrompt" or "Chr_PlayerCrouch" or
            "FollowCameraModifier" or "GetCurrentCameraTarget" or "GetCurrentCameraFov";

    private static bool IsEnvironmentPrimitive(string name) => ContainsAnyCanonical(name,
        "Logic", "Trigger", "GlobalEvent", "Interaction", "PressurePad", "Proximity",
        "MoveInTime", "RotateInTime", "PlayEnvironmentAnimation", "EnvironmentAnimation",
        "Lighting", "Light", "Visibility", "CameraShake", "Smoke", "Steam", "Rewire",
        "Sound", "Music", "AssetSpawner", "Zone", "StreamingMonitor", "Door", "Shutter",
        "Iris", "Lift", "Elevator", "Generator", "Power", "PhysicsApply", "Explosion",
        "BroadcastTrigger", "HostOnlyTrigger", "SyncOn", "NetworkedTimer");

    private static bool IsNondeterministicBoundary(string name) => ContainsAnyCanonical(name,
        "Random", "Weighted", "FilterIsLocalPlayer", "LocalPlayer");

    private static bool IsZonePrimitive(string name) => ContainsAnyCanonical(name,
        "ZoneEntity", "ZoneLink", "ZoneExclusionLinkEntity", "ZoneLoadedEntity", "Zone", "StreamingMonitor");

    private static bool ContainsAny(string value, params string[] needles) =>
        needles.Any(needle => value.Contains(needle, StringComparison.OrdinalIgnoreCase));

    private static bool ContainsAnyCanonical(string value, params string[] needles)
    {
        var canonicalValue = CanonicalToken(value);
        return needles.Any(needle => canonicalValue.Contains(CanonicalToken(needle), StringComparison.OrdinalIgnoreCase));
    }

    private static string CanonicalToken(string value)
    {
        if (string.IsNullOrEmpty(value)) return string.Empty;
        var buffer = new char[value.Length];
        var length = 0;
        foreach (var character in value)
        {
            if (!char.IsLetterOrDigit(character)) continue;
            buffer[length++] = char.ToUpperInvariant(character);
        }
        return new string(buffer, 0, length);
    }

    internal static string ResolveWorldDirectory(string levelOrWorldPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(levelOrWorldPath);
        var fullPath = Path.GetFullPath(levelOrWorldPath);

        if (File.Exists(fullPath))
        {
            var fileName = Path.GetFileName(fullPath);
            if (!fileName.Equals("COMMANDS.PAK", StringComparison.OrdinalIgnoreCase) &&
                !fileName.Equals("COMMANDS.BIN", StringComparison.OrdinalIgnoreCase))
            {
                throw new ArgumentException("A manifest source file must be COMMANDS.PAK or COMMANDS.BIN.", nameof(levelOrWorldPath));
            }
            return Path.GetDirectoryName(fullPath) ?? throw new InvalidOperationException("COMMANDS source has no parent directory.");
        }

        if (!Directory.Exists(fullPath)) throw new DirectoryNotFoundException(fullPath);
        if (File.Exists(Path.Combine(fullPath, "COMMANDS.PAK")) || File.Exists(Path.Combine(fullPath, "COMMANDS.BIN"))) return fullPath;

        var world = RipleyCharacterInstaller.ResolveWorldDirectory(fullPath);
        if (File.Exists(Path.Combine(world, "COMMANDS.PAK")) || File.Exists(Path.Combine(world, "COMMANDS.BIN"))) return world;

        throw new DirectoryNotFoundException($"'{fullPath}' does not contain WORLD/COMMANDS.PAK or WORLD/COMMANDS.BIN.");
    }

    internal static string GetDefaultCachePath(WorldEntityId mapContentId)
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return Path.Combine(root, "AIMP", "WorldManifestCache", $"{mapContentId}.aimpwm");
    }

    private static void WriteJson(string path, WorldManifest manifest)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        var options = new JsonSerializerOptions { WriteIndented = true };
        File.WriteAllText(path, JsonSerializer.Serialize(manifest, options));
    }

    private static bool PathsEqual(string left, string right) =>
        string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);

    private static uint EntryGuid(IReadOnlyList<Composite> entries, int index) =>
        index < entries.Count && entries[index] is not null ? entries[index].shortGUID.AsUInt32 : 0U;
}
