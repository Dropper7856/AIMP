using System.Security.Cryptography;
using System.Text;

namespace AIMP.Protocol;

public static class WorldManifestBinaryCodec
{
    private const string RuntimeCompatibilityMagic = "AIMPRC02";

    public static string ComputeRuntimeCompatibilitySha256(WorldManifest manifest)
    {
        ArgumentNullException.ThrowIfNull(manifest);

        using var stream = new MemoryStream();
        using (var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true))
        {
            writer.Write(Encoding.ASCII.GetBytes(RuntimeCompatibilityMagic));
            writer.Write(manifest.FormatVersion);
            writer.Write(manifest.CompilerVersion);
            writer.Write(manifest.MapContentIdHigh);
            writer.Write(manifest.MapContentIdLow);
            writer.Write(manifest.RootCompositeShortGuid);
            writer.Write(manifest.GlobalCompositeShortGuid);
            writer.Write(manifest.PauseMenuCompositeShortGuid);

            // The network compatibility contract intentionally hashes only the
            // fixed-width data consumed by the native WorldSync registry. This
            // excludes diagnostic strings/warnings/fingerprints whose bytes may
            // vary between fresh CathodeLib parses without changing runtime
            // identity or replay semantics.
            var entities = manifest.Entities
                .OrderBy(static value => value.EntityId.High)
                .ThenBy(static value => value.EntityId.Low)
                .ThenBy(static value => value.SourceCompositeShortGuid)
                .ThenBy(static value => value.SourceShortGuid)
                .ThenBy(static value => value.InstancePathId)
                .ToArray();
            WriteRuntimeIndex(writer, entities);

            var publicInputs = manifest.CompositePublicInputs
                .OrderBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ThenBy(static value => value.InternalTargetEntity.High)
                .ThenBy(static value => value.InternalTargetEntity.Low)
                .ThenBy(static value => value.InternalTargetInputShortGuid)
                .ThenBy(static value => value.InterfaceVariableEntity.High)
                .ThenBy(static value => value.InterfaceVariableEntity.Low)
                .ThenBy(static value => value.InterfaceLinkParameter)
                .ThenBy(static value => value.DataType)
                .ThenBy(static value => value.NoTypedData)
                .ThenBy(static value => value.InternalFallbackExclusive)
                .ToArray();
            WriteCompositePublicInputs(writer, publicInputs);

            var anchors = manifest.CompositePublicInputCanonicalAnchors
                .OrderBy(static value => value.InternalTargetEntity.High)
                .ThenBy(static value => value.InternalTargetEntity.Low)
                .ThenBy(static value => value.InternalTargetInputShortGuid)
                .ThenBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ToArray();
            WriteCompositePublicInputCanonicalAnchors(writer, anchors);

            var causalProfiles = manifest.CompositePublicInputCausalProfiles
                .OrderBy(static value => value.OwnerCompositeInstance.High)
                .ThenBy(static value => value.OwnerCompositeInstance.Low)
                .ThenBy(static value => value.PublicInputShortGuid)
                .ThenBy(static value => value.EndpointKind)
                .ToArray();
            WriteCompositePublicInputCausalProfiles(writer, causalProfiles);
        }

        return Convert.ToHexString(SHA256.HashData(stream.ToArray()));
    }

    public static void Write(string path, WorldManifest manifest)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(manifest);

        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: false);

        writer.Write(Encoding.ASCII.GetBytes(WorldManifestConstants.BinaryMagic));
        writer.Write(manifest.FormatVersion);
        writer.Write(manifest.CompilerVersion);
        writer.Write(manifest.MapContentIdHigh);
        writer.Write(manifest.MapContentIdLow);
        writer.Write(manifest.RootCompositeShortGuid);
        writer.Write(manifest.GlobalCompositeShortGuid);
        writer.Write(manifest.PauseMenuCompositeShortGuid);

        // v12.3 runtime index: fixed-width records placed before all variable-length
        // metadata so the injected x86 DLL can load the registry without parsing
        // strings, connections, aliases or diagnostic fields.
        WriteRuntimeIndex(writer, manifest.Entities);
        // Fix3.24 keeps the public Composite-input bridge in the fixed-width
        // runtime prefix too, so the injected DLL never parses strings or the
        // full static graph on the game thread.
        WriteCompositePublicInputs(writer, manifest.CompositePublicInputs);
        // BuildFix05 keeps compiler-generated descendant canonicalization anchors
        // in the same fixed-width runtime prefix. The DLL still does no graph scan.
        WriteCompositePublicInputCanonicalAnchors(
            writer, manifest.CompositePublicInputCanonicalAnchors);
        // WM09 causal/relevance/semantic-contract profiles are part of the
        // fixed-width runtime prefix. Native WS3 selects a semantic World
        // boundary and its represented sink in O(1) without parsing the full
        // static graph on the game thread.
        WriteCompositePublicInputCausalProfiles(
            writer, manifest.CompositePublicInputCausalProfiles);

        WriteSource(writer, manifest.Source);
        WriteComposites(writer, manifest.Composites);
        WritePaths(writer, manifest.InstancePaths);
        WriteEntities(writer, manifest.Entities);
        WriteConnections(writer, manifest.Connections);
        WriteOverrides(writer, manifest.Overrides);
        WriteReferences(writer, manifest.References);
        WriteZones(writer, manifest.Zones);

        writer.Write(manifest.ExcludedAimpCompositeCount);
        writer.Write(manifest.ExcludedAimpEntityCount);
        WriteStrings(writer, manifest.Warnings);
    }

    public static WorldManifest Read(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);

        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);

        var magic = Encoding.ASCII.GetString(reader.ReadBytes(WorldManifestConstants.BinaryMagic.Length));
        if (!string.Equals(magic, WorldManifestConstants.BinaryMagic, StringComparison.Ordinal))
        {
            throw new InvalidDataException($"Invalid WorldManifest magic '{magic}'.");
        }

        var formatVersion = reader.ReadUInt32();
        if (formatVersion != WorldManifestConstants.FormatVersion)
        {
            throw new InvalidDataException($"Unsupported WorldManifest format {formatVersion}.");
        }

        var compilerVersion = reader.ReadUInt32();
        var mapHigh = reader.ReadUInt64();
        var mapLow = reader.ReadUInt64();
        var root = reader.ReadUInt32();
        var global = reader.ReadUInt32();
        var pause = reader.ReadUInt32();

        var runtimeIndex = ReadRuntimeIndex(reader);
        var compositePublicInputs = ReadCompositePublicInputs(reader);
        var compositePublicInputCanonicalAnchors =
            ReadCompositePublicInputCanonicalAnchors(reader);
        var compositePublicInputCausalProfiles =
            ReadCompositePublicInputCausalProfiles(reader);

        var source = ReadSource(reader);
        var composites = ReadComposites(reader);
        var paths = ReadPaths(reader);
        var entities = ReadEntities(reader);
        var connections = ReadConnections(reader);
        var overrides = ReadOverrides(reader);
        var references = ReadReferences(reader);
        var zones = ReadZones(reader);
        var excludedComposites = reader.ReadUInt32();
        var excludedEntities = reader.ReadUInt32();
        var warnings = ReadStrings(reader);

        ValidateRuntimeIndex(runtimeIndex, entities);
        ValidateCompositePublicInputs(compositePublicInputs, entities);
        ValidateCompositePublicInputCanonicalAnchors(
            compositePublicInputCanonicalAnchors, compositePublicInputs, entities);
        ValidateCompositePublicInputCausalProfiles(
            compositePublicInputCausalProfiles, compositePublicInputs, entities);

        if (stream.Position != stream.Length)
        {
            throw new InvalidDataException("WorldManifest contains trailing bytes.");
        }

        return new WorldManifest(
            formatVersion,
            compilerVersion,
            mapHigh,
            mapLow,
            root,
            global,
            pause,
            source,
            composites,
            paths,
            entities,
            compositePublicInputs,
            compositePublicInputCanonicalAnchors,
            compositePublicInputCausalProfiles,
            connections,
            overrides,
            references,
            zones,
            excludedComposites,
            excludedEntities,
            warnings);
    }

    private readonly record struct RuntimeIndexEntry(
        WorldEntityId EntityId,
        uint CathodeEntityA,
        uint CathodeEntityB,
        uint ZoneId,
        WorldEntityFlags Flags,
        WorldDomain Domain,
        WorldEntityKind Kind,
        uint PrimitiveType);

    private static void WriteRuntimeIndex(
        BinaryWriter writer,
        IReadOnlyList<WorldEntityDescriptor> entities)
    {
        var runtimeEntities = entities
            .Where(static entity => entity.Flags.HasFlag(WorldEntityFlags.RuntimeAddressable))
            .ToArray();
        writer.Write(CheckedCount(runtimeEntities.Length));
        foreach (var entity in runtimeEntities)
        {
            WriteEntityId(writer, entity.EntityId);
            writer.Write(entity.CathodeEntityA);
            writer.Write(entity.CathodeEntityB);
            writer.Write(entity.ZoneId);
            writer.Write((ushort)entity.Flags);
            writer.Write((byte)entity.Domain);
            writer.Write((byte)entity.Kind);
            writer.Write(entity.PrimitiveType);
        }
    }

    private static RuntimeIndexEntry[] ReadRuntimeIndex(BinaryReader reader)
    {
        var result = new RuntimeIndexEntry[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new RuntimeIndexEntry(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                (WorldEntityFlags)reader.ReadUInt16(),
                (WorldDomain)reader.ReadByte(),
                (WorldEntityKind)reader.ReadByte(),
                reader.ReadUInt32());
        }
        return result;
    }

    private static void ValidateRuntimeIndex(
        IReadOnlyList<RuntimeIndexEntry> runtimeIndex,
        IReadOnlyList<WorldEntityDescriptor> entities)
    {
        var expected = new Dictionary<WorldEntityId, WorldEntityDescriptor>();
        var allEntityIds = new HashSet<WorldEntityId>();
        foreach (var entity in entities)
        {
            if (!allEntityIds.Add(entity.EntityId))
                throw new InvalidDataException("WorldManifest entity metadata contains a duplicate WorldEntityId.");
            if (!entity.Flags.HasFlag(WorldEntityFlags.RuntimeAddressable))
                continue;
            expected.Add(entity.EntityId, entity);
        }
        if (runtimeIndex.Count != expected.Count)
            throw new InvalidDataException("WorldManifest runtime index/runtime-addressable entity count mismatch.");

        var observed = new HashSet<WorldEntityId>();
        foreach (var entry in runtimeIndex)
        {
            if (!observed.Add(entry.EntityId)
                || !expected.TryGetValue(entry.EntityId, out var entity)
                || entity.CathodeEntityA != entry.CathodeEntityA
                || entity.CathodeEntityB != entry.CathodeEntityB
                || entity.ZoneId != entry.ZoneId
                || entity.Flags != entry.Flags
                || entity.Domain != entry.Domain
                || entity.Kind != entry.Kind
                || entity.PrimitiveType != entry.PrimitiveType)
            {
                throw new InvalidDataException("WorldManifest runtime index does not match entity metadata.");
            }
        }
    }

    private static void WriteCompositePublicInputs(
        BinaryWriter writer,
        IReadOnlyList<WorldCompositePublicInputDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.OwnerCompositeInstance);
            WriteEntityId(writer, value.InterfaceVariableEntity);
            writer.Write(value.PublicInputShortGuid);
            writer.Write(value.InterfaceLinkParameter);
            WriteEntityId(writer, value.InternalTargetEntity);
            writer.Write(value.InternalTargetInputShortGuid);
            writer.Write(value.DataType);
            writer.Write(value.NoTypedData ? (byte)1 : (byte)0);
            writer.Write(value.InternalFallbackExclusive ? (byte)1 : (byte)0);
            writer.Write((ushort)0);
        }
    }

    private static WorldCompositePublicInputDescriptor[] ReadCompositePublicInputs(BinaryReader reader)
    {
        var result = new WorldCompositePublicInputDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            var owner = ReadEntityId(reader);
            var interfaceVariable = ReadEntityId(reader);
            var publicInput = reader.ReadUInt32();
            var interfaceLinkParameter = reader.ReadUInt32();
            var internalTarget = ReadEntityId(reader);
            var internalTargetInput = reader.ReadUInt32();
            var dataType = reader.ReadUInt32();
            var noTypedData = reader.ReadByte();
            var internalFallbackExclusive = reader.ReadByte();
            var reserved16 = reader.ReadUInt16();
            if (noTypedData > 1 || internalFallbackExclusive > 1 || reserved16 != 0)
                throw new InvalidDataException("WorldManifest Composite public-input bridge contains invalid flags/reserved bytes.");
            result[index] = new WorldCompositePublicInputDescriptor(
                owner, interfaceVariable, publicInput, interfaceLinkParameter,
                internalTarget, internalTargetInput, dataType, noTypedData != 0,
                internalFallbackExclusive != 0);
        }
        return result;
    }

    private static void WriteCompositePublicInputCanonicalAnchors(
        BinaryWriter writer,
        IReadOnlyList<WorldCompositePublicInputCanonicalAnchor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.OwnerCompositeInstance);
            writer.Write(value.PublicInputShortGuid);
            WriteEntityId(writer, value.InternalTargetEntity);
            writer.Write(value.InternalTargetInputShortGuid);
        }
    }

    private static WorldCompositePublicInputCanonicalAnchor[]
        ReadCompositePublicInputCanonicalAnchors(BinaryReader reader)
    {
        var result = new WorldCompositePublicInputCanonicalAnchor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldCompositePublicInputCanonicalAnchor(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                ReadEntityId(reader),
                reader.ReadUInt32());
        }
        return result;
    }

    private static void WriteCompositePublicInputCausalProfiles(
        BinaryWriter writer,
        IReadOnlyList<WorldCompositePublicInputCausalProfile> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.OwnerCompositeInstance);
            writer.Write(value.PublicInputShortGuid);
            writer.Write((uint)value.DependencyFlags);
            writer.Write(value.EndpointKind);
            writer.Write(value.ReplicationRelevance);
            writer.Write((uint)value.ReplicationContractKind);
            WriteEntityId(writer, value.SemanticTargetEntity);
            writer.Write(value.SemanticOperation);
        }
    }

    private static WorldCompositePublicInputCausalProfile[] ReadCompositePublicInputCausalProfiles(
        BinaryReader reader)
    {
        var result = new WorldCompositePublicInputCausalProfile[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldCompositePublicInputCausalProfile(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                (WorldCausalDependencyFlags)reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                (WorldReplicationContractKind)reader.ReadUInt32(),
                ReadEntityId(reader),
                reader.ReadUInt32());
        }
        return result;
    }

    private static void ValidateCompositePublicInputCausalProfiles(
        IReadOnlyList<WorldCompositePublicInputCausalProfile> values,
        IReadOnlyList<WorldCompositePublicInputDescriptor> publicInputs,
        IReadOnlyList<WorldEntityDescriptor> entities)
    {
        const WorldCausalDependencyFlags known =
            WorldCausalDependencyFlags.WorldEffect |
            WorldCausalDependencyFlags.PlayerDependency |
            WorldCausalDependencyFlags.AiDependency |
            WorldCausalDependencyFlags.UiDependency |
            WorldCausalDependencyFlags.AimpDependency |
            WorldCausalDependencyFlags.UnknownDependency |
            WorldCausalDependencyFlags.Truncated | WorldCausalDependencyFlags.Branching;
        var owners = entities
            .Where(static value => value.Kind == WorldEntityKind.CompositeInstance)
            .Select(static value => value.EntityId)
            .ToHashSet();
        var publicKeys = publicInputs
            .Select(static value => (value.OwnerCompositeInstance, value.PublicInputShortGuid))
            .ToHashSet();
        var runtimeIds = entities.Where(static e => (e.Flags & WorldEntityFlags.RuntimeAddressable) != 0)
            .Select(static e => e.EntityId).ToHashSet();
        var exact = new HashSet<(WorldEntityId Owner, uint Input, uint Kind)>();
        var inputCount = 0;
        foreach (var value in values)
        {
            if (!value.OwnerCompositeInstance.IsValid || value.PublicInputShortGuid == 0U ||
                (value.DependencyFlags & ~known) != 0 ||
                !WorldReplicationRelevanceCodec.IsValid(value.ReplicationRelevance) ||
                (uint)value.ReplicationContractKind > (uint)WorldReplicationContractKind.StateTransition ||
                (value.ReplicationContractKind == WorldReplicationContractKind.Unknown
                    ? value.SemanticTargetEntity.IsValid || value.SemanticOperation != 0U
                    : !value.SemanticTargetEntity.IsValid || value.SemanticOperation == 0U ||
                      !runtimeIds.Contains(value.SemanticTargetEntity)) ||
                (value.EndpointKind == 1U
                    ? !owners.Contains(value.OwnerCompositeInstance) ||
                      !publicKeys.Contains((value.OwnerCompositeInstance, value.PublicInputShortGuid))
                    : (value.EndpointKind != 2U && value.EndpointKind != 3U) || !runtimeIds.Contains(value.OwnerCompositeInstance)) ||
                !exact.Add((value.OwnerCompositeInstance, value.PublicInputShortGuid, value.EndpointKind)))
            {
                throw new InvalidDataException(
                    "WorldManifest Composite public-input causal profile contains incompatible metadata.");
            }
        }
        inputCount = values.Count(static profile => profile.EndpointKind == 1U);
        if (inputCount != publicKeys.Count)
        {
            throw new InvalidDataException(
                "WorldManifest Composite public-input causal profiles do not cover every public input.");
        }
    }

    private static void ValidateCompositePublicInputCanonicalAnchors(
        IReadOnlyList<WorldCompositePublicInputCanonicalAnchor> values,
        IReadOnlyList<WorldCompositePublicInputDescriptor> publicInputs,
        IReadOnlyList<WorldEntityDescriptor> entities)
    {
        var byId = entities.ToDictionary(static value => value.EntityId);
        var allowed = publicInputs
            .Where(static value => value.NoTypedData)
            .Select(static value => (value.OwnerCompositeInstance, value.PublicInputShortGuid))
            .ToHashSet();
        var exact = new HashSet<(WorldEntityId Entity, uint Input)>();
        foreach (var value in values)
        {
            if (!value.OwnerCompositeInstance.IsValid ||
                value.PublicInputShortGuid == 0U ||
                !value.InternalTargetEntity.IsValid ||
                value.InternalTargetInputShortGuid == 0U)
            {
                throw new InvalidDataException(
                    "WorldManifest Composite public-input canonical anchor contains an invalid identity.");
            }
            if (!byId.TryGetValue(value.OwnerCompositeInstance, out var owner) ||
                owner.Kind != WorldEntityKind.CompositeInstance ||
                !byId.ContainsKey(value.InternalTargetEntity) ||
                !allowed.Contains((value.OwnerCompositeInstance, value.PublicInputShortGuid)))
            {
                throw new InvalidDataException(
                    "WorldManifest Composite public-input canonical anchor references incompatible metadata.");
            }
            if (!exact.Add((value.InternalTargetEntity, value.InternalTargetInputShortGuid)))
            {
                throw new InvalidDataException(
                    "WorldManifest Composite public-input canonical anchor contains a duplicate endpoint.");
            }
        }
    }

    private static void ValidateCompositePublicInputs(
        IReadOnlyList<WorldCompositePublicInputDescriptor> values,
        IReadOnlyList<WorldEntityDescriptor> entities)
    {
        var byId = entities.ToDictionary(static value => value.EntityId);
        var exact = new HashSet<(WorldEntityId Owner, WorldEntityId Variable, uint PublicInput, uint LinkParameter, WorldEntityId Target, uint TargetInput)>();
        foreach (var value in values)
        {
            if (!value.OwnerCompositeInstance.IsValid || !value.InterfaceVariableEntity.IsValid ||
                !value.InternalTargetEntity.IsValid || value.PublicInputShortGuid == 0 ||
                (!value.NoTypedData && value.InterfaceLinkParameter == 0) ||
                value.InternalTargetInputShortGuid == 0)
            {
                throw new InvalidDataException("WorldManifest Composite public-input bridge contains an invalid identity.");
            }
            if (!byId.TryGetValue(value.OwnerCompositeInstance, out var owner) ||
                owner.Kind != WorldEntityKind.CompositeInstance ||
                !byId.TryGetValue(value.InterfaceVariableEntity, out var variable) ||
                variable.Kind != WorldEntityKind.Variable ||
                !byId.ContainsKey(value.InternalTargetEntity))
            {
                throw new InvalidDataException("WorldManifest Composite public-input bridge references incompatible entity metadata.");
            }
            if (!exact.Add((value.OwnerCompositeInstance, value.InterfaceVariableEntity,
                    value.PublicInputShortGuid, value.InterfaceLinkParameter,
                    value.InternalTargetEntity, value.InternalTargetInputShortGuid)))
            {
                throw new InvalidDataException("WorldManifest Composite public-input bridge contains a duplicate direct link.");
            }
        }
    }

    private static void WriteSource(BinaryWriter writer, WorldManifestSourceInfo source)
    {
        writer.Write(source.CommandsPakSha256 ?? string.Empty);
        writer.Write(source.CommandsBinSha256 ?? string.Empty);
        writer.Write(source.CommandsPakPresent);
        writer.Write(source.CommandsBinPresent);
        writer.Write(source.SourcesStructurallyAgree);
        writer.Write(source.StructuralFingerprintPak ?? string.Empty);
        writer.Write(source.StructuralFingerprintBin ?? string.Empty);
    }

    private static WorldManifestSourceInfo ReadSource(BinaryReader reader) => new(
        reader.ReadString(),
        reader.ReadString(),
        reader.ReadBoolean(),
        reader.ReadBoolean(),
        reader.ReadBoolean(),
        reader.ReadString(),
        reader.ReadString());

    private static void WriteComposites(BinaryWriter writer, IReadOnlyList<WorldCompositeDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            writer.Write(value.CompositeShortGuid);
            writer.Write(value.Name ?? string.Empty);
            writer.Write((byte)value.Domain);
            writer.Write(value.AimpNamespace);
            writer.Write(value.Instantiated);
        }
    }

    private static WorldCompositeDescriptor[] ReadComposites(BinaryReader reader)
    {
        var result = new WorldCompositeDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldCompositeDescriptor(
                reader.ReadUInt32(),
                reader.ReadString(),
                (WorldDomain)reader.ReadByte(),
                reader.ReadBoolean(),
                reader.ReadBoolean());
        }
        return result;
    }

    private static void WritePaths(BinaryWriter writer, IReadOnlyList<WorldInstancePathDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            writer.Write(value.PathId);
            writer.Write(value.HierarchyHash);
            writer.Write(value.CathodeHierarchyId);
            writer.Write(CheckedCount(value.Steps.Count));
            foreach (var step in value.Steps) writer.Write(step);
        }
    }

    private static WorldInstancePathDescriptor[] ReadPaths(BinaryReader reader)
    {
        var result = new WorldInstancePathDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            var pathId = reader.ReadUInt32();
            var hierarchyHash = reader.ReadUInt64();
            var cathodeHierarchyId = reader.ReadUInt32();
            var steps = new uint[ReadCount(reader)];
            for (var step = 0; step < steps.Length; step++) steps[step] = reader.ReadUInt32();
            result[index] = new WorldInstancePathDescriptor(pathId, steps, hierarchyHash, cathodeHierarchyId);
        }
        return result;
    }

    private static void WriteEntities(BinaryWriter writer, IReadOnlyList<WorldEntityDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.EntityId);
            writer.Write((byte)value.Domain);
            writer.Write((byte)value.Kind);
            writer.Write((ushort)value.Flags);
            writer.Write(value.SourceCompositeShortGuid);
            writer.Write(value.SourceShortGuid);
            writer.Write(value.InstancePathId);
            writer.Write(value.HierarchyHash);
            writer.Write(value.CathodeEntityA);
            writer.Write(value.CathodeEntityB);
            writer.Write(value.ZoneId);
            writer.Write(value.PrimitiveType);
            writer.Write(value.PrimitiveName ?? string.Empty);
            writer.Write(value.EntityName ?? string.Empty);
        }
    }

    private static WorldEntityDescriptor[] ReadEntities(BinaryReader reader)
    {
        var result = new WorldEntityDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldEntityDescriptor(
                ReadEntityId(reader),
                (WorldDomain)reader.ReadByte(),
                (WorldEntityKind)reader.ReadByte(),
                (WorldEntityFlags)reader.ReadUInt16(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt64(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadString(),
                reader.ReadString());
        }
        return result;
    }

    private static void WriteConnections(BinaryWriter writer, IReadOnlyList<WorldConnectionDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.SourceEntity);
            writer.Write(value.SourceParameter);
            WriteEntityId(writer, value.TargetEntity);
            writer.Write(value.TargetParameter);
            writer.Write(value.PotentialCausalEdge);
        }
    }

    private static WorldConnectionDescriptor[] ReadConnections(BinaryReader reader)
    {
        var result = new WorldConnectionDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldConnectionDescriptor(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                ReadEntityId(reader),
                reader.ReadUInt32(),
                reader.ReadBoolean());
        }
        return result;
    }

    private static void WriteOverrides(BinaryWriter writer, IReadOnlyList<WorldOverrideDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.OwnerCompositeInstance);
            writer.Write(value.ParameterName);
            writer.Write(value.DataType);
            writer.Write(value.ParameterVariant);
        }
    }

    private static WorldOverrideDescriptor[] ReadOverrides(BinaryReader reader)
    {
        var result = new WorldOverrideDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldOverrideDescriptor(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadUInt32());
        }
        return result;
    }


    private static void WriteReferences(BinaryWriter writer, IReadOnlyList<WorldReferenceDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.OwnerEntity);
            writer.Write((byte)value.Kind);
            writer.Write(value.FunctionType);
            writer.Write(CheckedCount(value.TargetPathSteps.Count));
            foreach (var step in value.TargetPathSteps) writer.Write(step);
        }
    }

    private static WorldReferenceDescriptor[] ReadReferences(BinaryReader reader)
    {
        var result = new WorldReferenceDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            var owner = ReadEntityId(reader);
            var kind = (WorldReferenceKind)reader.ReadByte();
            var functionType = reader.ReadUInt32();
            var steps = new uint[ReadCount(reader)];
            for (var step = 0; step < steps.Length; step++) steps[step] = reader.ReadUInt32();
            result[index] = new WorldReferenceDescriptor(owner, kind, functionType, steps);
        }
        return result;
    }

    private static void WriteZones(BinaryWriter writer, IReadOnlyList<WorldZoneDescriptor> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values)
        {
            WriteEntityId(writer, value.ZoneEntity);
            writer.Write(value.ZoneId);
            writer.Write(value.InstancePathId);
            writer.Write(value.EntityName ?? string.Empty);
        }
    }

    private static WorldZoneDescriptor[] ReadZones(BinaryReader reader)
    {
        var result = new WorldZoneDescriptor[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++)
        {
            result[index] = new WorldZoneDescriptor(
                ReadEntityId(reader),
                reader.ReadUInt32(),
                reader.ReadUInt32(),
                reader.ReadString());
        }
        return result;
    }

    private static void WriteStrings(BinaryWriter writer, IReadOnlyList<string> values)
    {
        writer.Write(CheckedCount(values.Count));
        foreach (var value in values) writer.Write(value ?? string.Empty);
    }

    private static string[] ReadStrings(BinaryReader reader)
    {
        var result = new string[ReadCount(reader)];
        for (var index = 0; index < result.Length; index++) result[index] = reader.ReadString();
        return result;
    }

    private static void WriteEntityId(BinaryWriter writer, WorldEntityId value)
    {
        writer.Write(value.High);
        writer.Write(value.Low);
    }

    private static WorldEntityId ReadEntityId(BinaryReader reader) =>
        new(reader.ReadUInt64(), reader.ReadUInt64());

    private static int CheckedCount(int count)
    {
        if (count < 0) throw new InvalidDataException("Negative collection count.");
        return count;
    }

    private static int ReadCount(BinaryReader reader)
    {
        var count = reader.ReadInt32();
        if (count < 0 || count > 10_000_000)
        {
            throw new InvalidDataException($"Invalid WorldManifest collection count {count}.");
        }
        return count;
    }
}
