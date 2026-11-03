namespace AIMP.Protocol;

public static class WorldManifestConstants
{
    public const uint FormatVersion = 9;
    public const uint CompilerVersion = 0x00100010; // Exact emitted Door force-event carriers
    public const string BinaryMagic = "AIMPWM09";
}

public static class WorldManifestRuntimeIdentity
{
    // CathodeLib/OpenCAGE proves that the world/root instance tree is anchored
    // by ShortGuid.InstanceGuid. No equivalent runtime-instance anchor is assumed
    // for the other COMMANDS entry points until independently proven.
    public const uint RootInstanceBaseId = 0x4AF05BFEU;

    public static uint ComputeCathodeHierarchyId(IReadOnlyList<uint> path)
    {
        ArgumentNullException.ThrowIfNull(path);
        var value = 0U;
        for (var index = path.Count - 1; index >= 0; index--)
            value = CombineCathodeHierarchy(value, path[index]);
        return value;
    }

    public static uint ComputeCathodeCompositeInstanceId(IReadOnlyList<uint> path, uint baseInstanceId)
    {
        ArgumentNullException.ThrowIfNull(path);
        if (baseInstanceId == 0U)
            throw new ArgumentOutOfRangeException(nameof(baseInstanceId), "CATHODE composite-instance base ID cannot be zero.");

        // EntityPath.GenerateCompositeInstanceID(false) folds the FunctionEntity
        // instance path first, then anchors that hierarchy to the entry-point
        // composite-instance ID. CombineCathodeHierarchy mirrors FUN_008BF3D0.
        return CombineCathodeHierarchy(ComputeCathodeHierarchyId(path), baseInstanceId);
    }

    public static uint CombineCathodeHierarchy(uint current, uint next)
    {
        if (next == 0U) return current;
        if (current == 0U) return next;

        unchecked
        {
            ulong value = ((ulong)current << 32) | next;
            value = (value << 18) + ~value;
            value = ((value >> 31) ^ value) * 21UL;
            value = (value >> 11) ^ value;
            value *= 65UL;
            return (uint)((value >> 22) ^ value);
        }
    }
}

public enum WorldDomain : byte
{
    Unknown = 0,
    World,
    AI,
    Player,
    UI,
    AIMP
}

public enum WorldEntityKind : byte
{
    Unknown = 0,
    Function,
    Variable,
    Proxy,
    Alias,
    CompositeInstance,
    Override
}

public enum WorldReferenceKind : byte
{
    Proxy = 0,
    Alias
}

[Flags]
public enum WorldEntityFlags : ushort
{
    None = 0,
    EnvironmentPrimitive = 1 << 0,
    NondeterministicBoundary = 1 << 1,
    ZonePrimitive = 1 << 2,
    CompositeInstance = 1 << 3,
    HasOutgoingLinks = 1 << 4,
    HasOverrides = 1 << 5,
    SourceFromPak = 1 << 6,
    SourceFromBin = 1 << 7,
    RuntimeAddressable = 1 << 8,
    PlayerMissionComplete = 1 << 9
}

public readonly record struct WorldEntityId(ulong High, ulong Low)
{
    public bool IsValid => High != 0 || Low != 0;

    public override string ToString() => $"{High:X16}{Low:X16}";
}

public sealed record WorldCompositeDescriptor(
    uint CompositeShortGuid,
    string Name,
    WorldDomain Domain,
    bool AimpNamespace,
    bool Instantiated);

public sealed record WorldInstancePathDescriptor(
    uint PathId,
    IReadOnlyList<uint> Steps,
    ulong HierarchyHash,
    uint CathodeHierarchyId);

public sealed record WorldEntityDescriptor(
    WorldEntityId EntityId,
    WorldDomain Domain,
    WorldEntityKind Kind,
    WorldEntityFlags Flags,
    uint SourceCompositeShortGuid,
    uint SourceShortGuid,
    uint InstancePathId,
    ulong HierarchyHash,
    uint CathodeEntityA,
    uint CathodeEntityB,
    uint ZoneId,
    uint PrimitiveType,
    string PrimitiveName,
    string EntityName);

public sealed record WorldConnectionDescriptor(
    WorldEntityId SourceEntity,
    uint SourceParameter,
    WorldEntityId TargetEntity,
    uint TargetParameter,
    bool PotentialCausalEdge);

// Fix3.24 manifest-only structural bridge. CathodeLib VariableEntity.name is
// the public Composite parameter ShortGuid; the child link supplies the direct
// internal target. No runtime pointer or gameplay-specific semantic is stored.
public sealed record WorldCompositePublicInputDescriptor(
    WorldEntityId OwnerCompositeInstance,
    WorldEntityId InterfaceVariableEntity,
    uint PublicInputShortGuid,
    uint InterfaceLinkParameter,
    WorldEntityId InternalTargetEntity,
    uint InternalTargetInputShortGuid,
    uint DataType,
    bool NoTypedData,
    bool InternalFallbackExclusive);

// Fix3.24 BuildFix05 compiler-generated canonicalization anchor. This is not a
// replay target. It records only that a payloadless public Composite input is
// the unique statically reachable public set_false lineage for a downstream
// internal set_false endpoint. Runtime transport remains owner+public input.
public sealed record WorldCompositePublicInputCanonicalAnchor(
    WorldEntityId OwnerCompositeInstance,
    uint PublicInputShortGuid,
    WorldEntityId InternalTargetEntity,
    uint InternalTargetInputShortGuid);

[Flags]
public enum WorldCausalDependencyFlags : uint
{
    None = 0,
    WorldEffect = 1U << 0,
    PlayerDependency = 1U << 1,
    AiDependency = 1U << 2,
    UiDependency = 1U << 3,
    AimpDependency = 1U << 4,
    UnknownDependency = 1U << 5,
    Truncated = 1U << 6,
    Branching = 1U << 7
}

// WM09 retains the WM08 separation between domain reachability (WorldEffect)
// and network relevance, then adds an orthogonal semantic replication contract.
// The packed uint is intentionally fixed-width so native admission stays O(1).
public enum WorldReplicationRelevanceDisposition : uint
{
    Unknown = 0U,
    NotRelevant = 1U,
    DeferToDescendant = 2U,
    ReplicateIntrinsic = 3U,
    ReplicateCarrier = 4U
}

public enum WorldReplicationSemanticClass : uint
{
    Unknown = 0U,
    RoutingOnly = 1U,
    SchedulerMaintenanceOnly = 2U,
    InternalStateOnly = 3U,
    SharedWorldMutation = 4U,
    SharedPhysicsMutation = 5U,
    SharedLifecycleMutation = 6U,
    CompositeRouting = 7U
}

// Semantic replication intent is deliberately orthogonal to WorldMutationKind.
// A causal ROOT remains an authoritative occurrence (currently transported as a
// WorldMutationKind.Event) while this contract describes how a later receiver
// may reconcile the represented gameplay meaning before invoking retail CATHODE.
// Unknown is fail-closed once semantic reconciliation is activated in a later
// patch; WM09 Patch 1 transports metadata only and does not change execution.
public enum WorldReplicationContractKind : uint
{
    Unknown = 0U,
    Event = 1U,
    State = 2U,
    StateTransition = 3U
}

[Flags]
public enum WorldReplicationRelevanceEvidence : uint
{
    None = 0U,
    ReplicableSinkReachable = 1U << 8,
    RoutingOnly = 1U << 9,
    SchedulerMaintenance = 1U << 10,
    InternalStateOnly = 1U << 11,
    IntrinsicSharedEffect = 1U << 12,
    EffectBeforeOutput = 1U << 13,
    CompleteDescendantFrontier = 1U << 14,
    ReplayHistorySensitive = 1U << 15,
    CycleAmbiguous = 1U << 16,
    SemanticUnknown = 1U << 17,
    NoReplicableSink = 1U << 18,
    NativeHandlerEvidence = 1U << 19,
    StructuralGraphEvidence = 1U << 20,
    DirectUnhookedSink = 1U << 21,
    ExactInputSemantic = 1U << 22,
    // Patch 4: this captured output is the immediate certified cause before a
    // unique exact StateTransition ScheduledInput sink. It may be replayed as
    // the semantic carrier instead of replaying that lower consumer directly.
    StateTransitionCarrier = 1U << 23,
    // Patch 5: this public CompositeInput is a higher certified cause whose
    // complete relevant descendant frontier converges on one exact
    // StateTransition semantic target/operation. Replaying the public input
    // lets retail CATHODE regenerate the lower emitted/scheduled consequences.
    CompositeStateTransitionCarrier = 1U << 24,
    // Patch 5.1: the CompositeInput carrier was certified by convergence of all
    // known relevant semantic descendants while tolerating non-truncated
    // UnknownDependency side paths and cyclic routing. This intentionally opens
    // the high-level boundary without changing the fixed-width manifest layout.
    CompositeRelevantFrontierRelaxed = 1U << 25,
    EmittedEventCarrier = 1U << 26
}

public static class WorldReplicationRelevanceCodec
{
    public const uint DispositionMask = 0x7U;
    public const int SemanticClassShift = 3;
    public const uint SemanticClassMask = 0x1FU << SemanticClassShift;
    public const uint KnownEvidenceMask =
        (uint)(WorldReplicationRelevanceEvidence.ReplicableSinkReachable |
            WorldReplicationRelevanceEvidence.RoutingOnly |
            WorldReplicationRelevanceEvidence.SchedulerMaintenance |
            WorldReplicationRelevanceEvidence.InternalStateOnly |
            WorldReplicationRelevanceEvidence.IntrinsicSharedEffect |
            WorldReplicationRelevanceEvidence.EffectBeforeOutput |
            WorldReplicationRelevanceEvidence.CompleteDescendantFrontier |
            WorldReplicationRelevanceEvidence.ReplayHistorySensitive |
            WorldReplicationRelevanceEvidence.CycleAmbiguous |
            WorldReplicationRelevanceEvidence.SemanticUnknown |
            WorldReplicationRelevanceEvidence.NoReplicableSink |
            WorldReplicationRelevanceEvidence.NativeHandlerEvidence |
            WorldReplicationRelevanceEvidence.StructuralGraphEvidence |
            WorldReplicationRelevanceEvidence.DirectUnhookedSink |
            WorldReplicationRelevanceEvidence.ExactInputSemantic |
            WorldReplicationRelevanceEvidence.StateTransitionCarrier |
            WorldReplicationRelevanceEvidence.CompositeStateTransitionCarrier |
            WorldReplicationRelevanceEvidence.CompositeRelevantFrontierRelaxed |
            WorldReplicationRelevanceEvidence.EmittedEventCarrier);

    public static uint Pack(
        WorldReplicationRelevanceDisposition disposition,
        WorldReplicationSemanticClass semanticClass,
        WorldReplicationRelevanceEvidence evidence) =>
        ((uint)disposition & DispositionMask) |
        (((uint)semanticClass << SemanticClassShift) & SemanticClassMask) |
        ((uint)evidence & KnownEvidenceMask);

    public static WorldReplicationRelevanceDisposition Disposition(uint packed) =>
        (WorldReplicationRelevanceDisposition)(packed & DispositionMask);

    public static WorldReplicationSemanticClass SemanticClass(uint packed) =>
        (WorldReplicationSemanticClass)((packed & SemanticClassMask) >> SemanticClassShift);

    public static WorldReplicationRelevanceEvidence Evidence(uint packed) =>
        (WorldReplicationRelevanceEvidence)(packed & KnownEvidenceMask);

    public static bool ReplicationAllowed(uint packed) =>
        Disposition(packed) is WorldReplicationRelevanceDisposition.ReplicateIntrinsic or
            WorldReplicationRelevanceDisposition.ReplicateCarrier;

    public static bool IsValid(uint packed)
    {
        var disposition = Disposition(packed);
        var semantic = SemanticClass(packed);
        return (uint)disposition <= (uint)WorldReplicationRelevanceDisposition.ReplicateCarrier &&
            (uint)semantic <= (uint)WorldReplicationSemanticClass.CompositeRouting &&
            (packed & ~(DispositionMask | SemanticClassMask | KnownEvidenceMask)) == 0U;
    }
}

// EndpointKind 3: scheduled consumer MAY-summary. Kind 1: public input MAY-summary. Kind 2: one
// observed output port, indexed by (source entity, SourceParameter). Primitive
// input->output unions remain conservative; runtime emission selects the real
// branch. Domain metadata is inherited classification, not a native proof.
public sealed record WorldCompositePublicInputCausalProfile(
    WorldEntityId OwnerCompositeInstance,
    uint PublicInputShortGuid,
    WorldCausalDependencyFlags DependencyFlags,
    uint EndpointKind,
    uint ReplicationRelevance,
    WorldReplicationContractKind ReplicationContractKind,
    WorldEntityId SemanticTargetEntity,
    uint SemanticOperation);

public sealed record WorldOverrideDescriptor(
    WorldEntityId OwnerCompositeInstance,
    uint ParameterName,
    uint DataType,
    uint ParameterVariant);

public sealed record WorldReferenceDescriptor(
    WorldEntityId OwnerEntity,
    WorldReferenceKind Kind,
    uint FunctionType,
    IReadOnlyList<uint> TargetPathSteps);

public sealed record WorldZoneDescriptor(
    WorldEntityId ZoneEntity,
    uint ZoneId,
    uint InstancePathId,
    string EntityName);

public sealed record WorldManifestSourceInfo(
    string CommandsPakSha256,
    string CommandsBinSha256,
    bool CommandsPakPresent,
    bool CommandsBinPresent,
    bool SourcesStructurallyAgree,
    string StructuralFingerprintPak,
    string StructuralFingerprintBin);

public sealed record WorldManifest(
    uint FormatVersion,
    uint CompilerVersion,
    ulong MapContentIdHigh,
    ulong MapContentIdLow,
    uint RootCompositeShortGuid,
    uint GlobalCompositeShortGuid,
    uint PauseMenuCompositeShortGuid,
    WorldManifestSourceInfo Source,
    IReadOnlyList<WorldCompositeDescriptor> Composites,
    IReadOnlyList<WorldInstancePathDescriptor> InstancePaths,
    IReadOnlyList<WorldEntityDescriptor> Entities,
    IReadOnlyList<WorldCompositePublicInputDescriptor> CompositePublicInputs,
    IReadOnlyList<WorldCompositePublicInputCanonicalAnchor> CompositePublicInputCanonicalAnchors,
    IReadOnlyList<WorldCompositePublicInputCausalProfile> CompositePublicInputCausalProfiles,
    IReadOnlyList<WorldConnectionDescriptor> Connections,
    IReadOnlyList<WorldOverrideDescriptor> Overrides,
    IReadOnlyList<WorldReferenceDescriptor> References,
    IReadOnlyList<WorldZoneDescriptor> Zones,
    uint ExcludedAimpCompositeCount,
    uint ExcludedAimpEntityCount,
    IReadOnlyList<string> Warnings)
{
    public WorldEntityId MapContentId => new(MapContentIdHigh, MapContentIdLow);
}

public enum WorldMutationKind : byte
{
    Intent = 0,
    State,
    Event,
    Timeline
}

[Flags]
public enum WorldMutationFlags : byte
{
    None = 0,
    Reliable = 1 << 0,
    Replaceable = 1 << 1,
    Replayable = 1 << 2,
    SyntheticProbe = 1 << 3
}

public static class WorldReplicationConstants
{
    public const ushort SchemaVersion = 1;
    public const int MaximumPayloadBytes = 4096;
    public const int MaximumReplayStates = 4096;
    public const uint NetworkProbeOperation = 0xFFFF0001U;
    public const uint CathodeTriggerInputOperation = CathodePrimitiveCodec.TriggerInputOperation;
    public const uint ObservedCathodeInputEdgeOperation = WorldInteractionAdapterCodec.ObservedInputEdgeOperation;
    public const uint ObservedCathodeCallbackInputEdgeOperation = WorldInteractionAdapterCodec.ObservedCallbackInputEdgeOperation;
    public const uint ObservedCathodeCharacterCallbackInputEdgeOperation = WorldInteractionAdapterCodec.ObservedCharacterCallbackInputEdgeOperation;
    public const uint ObservedCathodeTypedMessageOperation = WorldCathodeBusCodec.TypedMessageOperation;
    public const uint ObservedCathodeLogicSwitchStateOperation = 0x000C0901U;
    // AIMP/WorldSync operation ID; not a retail CATHODE messageType.
    public const uint CompositePublicInputOperation = 0x000C0A01U;
    public const uint DoorActionOperation = WorldAnimatedAdapterCodec.DoorActionOperation;
    public const uint AnimatedInputEdgeOperation = WorldAnimatedAdapterCodec.AnimatedInputEdgeOperation;
}

public readonly record struct WorldManifestIdentity(
    uint FormatVersion,
    uint CompilerVersion,
    WorldEntityId MapContentId)
{
    public bool IsValid =>
        FormatVersion == WorldManifestConstants.FormatVersion
        && CompilerVersion == WorldManifestConstants.CompilerVersion
        && MapContentId.IsValid;
}

public sealed record WorldIntent(
    WorldManifestIdentity Manifest,
    WorldEntityId Entity,
    uint LevelGeneration,
    ulong SourcePeer,
    ulong ClientSequence,
    WorldMutationKind RequestedKind,
    uint Operation,
    byte[] Payload);

public sealed record WorldMutation(
    WorldManifestIdentity Manifest,
    WorldMutationKind Kind,
    WorldMutationFlags Flags,
    WorldEntityId Entity,
    uint LevelGeneration,
    ulong Sequence,
    ulong Revision,
    ulong TimelineTick,
    ulong SourcePeer,
    uint Operation,
    byte[] Payload);

public sealed record WorldState(
    WorldManifestIdentity Manifest,
    WorldEntityId Entity,
    uint LevelGeneration,
    ulong Revision,
    ulong TimelineTick,
    uint StateType,
    byte[] Payload);

public sealed record WorldEvent(
    WorldManifestIdentity Manifest,
    WorldEntityId Entity,
    uint LevelGeneration,
    ulong Sequence,
    ulong TimelineTick,
    ulong SourcePeer,
    uint EventType,
    byte[] Payload);

public sealed record WorldTimeline(
    WorldManifestIdentity Manifest,
    WorldEntityId Entity,
    uint LevelGeneration,
    ulong Revision,
    ulong TimelineTick,
    uint TimelineType,
    byte[] Payload);

public sealed record WorldNetworkContext(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    WorldManifestIdentity Manifest,
    // Canonical SHA-256 of the fixed-width runtime WorldSync contract. It is
    // intentionally not the SHA-256 of every byte in the .aimpwm file.
    string ManifestSha256,
    bool Ready);

public sealed record WorldNetworkIntent(
    Guid SessionId,
    int WorldGeneration,
    WorldIntent Intent);

public sealed record WorldNetworkMutation(
    Guid SessionId,
    int WorldGeneration,
    WorldMutation Mutation);

public enum WorldMutationResultCode : byte
{
    Accepted = 0,
    DuplicateRejected,
    StaleRejected,
    GenerationRejected,
    ManifestRejected,
    EntityRejected,
    QueueRejected,
    RuntimePending,
    NativeFault
}

public sealed record WorldMutationResult(
    ulong Sequence,
    WorldMutationResultCode Code,
    bool EntityResolved,
    bool Dispatched,
    // Local Launcher <-> DLL correlation only. Never serialized on multiplayer wire.
    ulong NativeDispatchId = 0);

public sealed record WorldNetworkMutationAck(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    WorldMutationResult Result);
