using System.Text.Json.Serialization;

namespace AIMP.Protocol;

public static class BuildResolutionModes
{
    public const string ExactProfile = nameof(ExactProfile);
    public const string LocalDiscoveredProfile = nameof(LocalDiscoveredProfile);
    public const string GenericDiscovery = nameof(GenericDiscovery);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[] { ExactProfile, LocalDiscoveredProfile, GenericDiscovery },
        StringComparer.Ordinal);
}

public static class NativeCapabilityNames
{
    public const string ObserveNetZ = nameof(ObserveNetZ);
    public const string ObserveCharacterRequests = nameof(ObserveCharacterRequests);
    public const string ObserveCharacterLifecycle = nameof(ObserveCharacterLifecycle);
    public const string ObserveParticipants = nameof(ObserveParticipants);
    public const string ObserveChannels = nameof(ObserveChannels);
    public const string ObserveReplicationContexts = nameof(ObserveReplicationContexts);
    public const string ObserveLevelTransitions = nameof(ObserveLevelTransitions);
    public const string ScheduleGameThreadWork = nameof(ScheduleGameThreadWork);
    public const string CreateDummyParticipant = nameof(CreateDummyParticipant);
    public const string ReplayCharacterSpawn = nameof(ReplayCharacterSpawn);
    public const string BindDummyCharacter = nameof(BindDummyCharacter);
    public const string DestroyDummyCharacter = nameof(DestroyDummyCharacter);
    public const string ObserveOutboundDescriptors = nameof(ObserveOutboundDescriptors);
    public const string ApplyInboundDescriptors = nameof(ApplyInboundDescriptors);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            ObserveNetZ,
            ObserveCharacterRequests,
            ObserveCharacterLifecycle,
            ObserveParticipants,
            ObserveChannels,
            ObserveReplicationContexts,
            ObserveLevelTransitions,
            ScheduleGameThreadWork,
            CreateDummyParticipant,
            ReplayCharacterSpawn,
            BindDummyCharacter,
            DestroyDummyCharacter,
            ObserveOutboundDescriptors,
            ApplyInboundDescriptors
        },
        StringComparer.Ordinal);
}

public static class CapabilityStateNames
{
    public const string Unresolved = nameof(Unresolved);
    public const string Observed = nameof(Observed);
    public const string StaticValidated = nameof(StaticValidated);
    public const string RuntimeValidated = nameof(RuntimeValidated);
    public const string Blocked = nameof(Blocked);
    public const string Disabled = nameof(Disabled);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            Unresolved,
            Observed,
            StaticValidated,
            RuntimeValidated,
            Blocked,
            Disabled
        },
        StringComparer.Ordinal);
}

public static class DummyStateNames
{
    public const string Absent = nameof(Absent);
    public const string Creating = nameof(Creating);
    public const string Active = nameof(Active);
    public const string Removing = nameof(Removing);
    public const string Failed = nameof(Failed);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[] { Absent, Creating, Active, Removing, Failed },
        StringComparer.Ordinal);
}

public static class DummyAchievementLevels
{
    public const string None = nameof(None);
    public const string A = nameof(A);
    public const string B = nameof(B);
    public const string C = nameof(C);
    public const string D = nameof(D);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[] { None, A, B, C, D },
        StringComparer.Ordinal);
}

public static class DummyMoveCommands
{
    public const string Forward = nameof(Forward);
    public const string Backward = nameof(Backward);
    public const string RotateLeft = nameof(RotateLeft);
    public const string RotateRight = nameof(RotateRight);
    public const string NearLocalPlayer = nameof(NearLocalPlayer);
    public const string MovementTest = nameof(MovementTest);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            Forward,
            Backward,
            RotateLeft,
            RotateRight,
            NearLocalPlayer,
            MovementTest
        },
        StringComparer.Ordinal);
}

public static class DummyMovementMethods
{
    public const string NotApplied = nameof(NotApplied);
    public const string NativeDescriptor = nameof(NativeDescriptor);
    public const string NativeLocomotion = nameof(NativeLocomotion);
    public const string DirectTransformDiagnostic = nameof(DirectTransformDiagnostic);
    public const string CathodeDiagnostic = nameof(CathodeDiagnostic);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            NotApplied,
            NativeDescriptor,
            NativeLocomotion,
            DirectTransformDiagnostic,
            CathodeDiagnostic
        },
        StringComparer.Ordinal);
}

public static class GameCommandNames
{
    public const string PopupMessage = nameof(PopupMessage);
    public const string SpawnCharacter = nameof(SpawnCharacter);
    public const string SpawnNetZCharacter = nameof(SpawnNetZCharacter);
    public const string CharacterDirectMove = "CHARACTER_DIRECT_MOVE";
    public const string CharacterDirectStep = "CHARACTER_DIRECT_STEP";
    public const string RemoteEquipmentProvision = "REMOTE_EQUIPMENT_PROVISION";
    public const string CharacterActionTest = "CHARACTER_ACTION_TEST";
    public const string CharacterAmmoRefill = "CHARACTER_AMMO_REFILL";
    public const string WorldRegistryProbe = "WORLD_REGISTRY_PROBE";
    public const string WorldManifestLoad = "WORLD_MANIFEST_LOAD";
    public const string WorldNetProbe = "WORLD_NET_PROBE";
    public const string WorldInteractionStats = "WORLD_INTERACTION_STATS";
    public const string WorldCompositeInputStats = "WORLD_COMPOSITE_INPUT_STATS";
    // Internal Launcher -> Native ProbeDirect configuration, intentionally absent from the UI catalogue.
    public const string WorldWs3EmptyCallbackSet = "WORLD_WS3_EMPTY_CALLBACK_SET";
    public const string PerformanceIsolationSet = "PERFORMANCE_ISOLATION_SET";
    public const string WorldStateDiscoveryBaseline = "WORLD_STATE_DISCOVERY_BASELINE";
    public const string WorldStateDiscoveryAfter = "WORLD_STATE_DISCOVERY_AFTER";
    public const string WorldStateDiscoveryStatus = "WORLD_STATE_DISCOVERY_STATUS";
    // Internal round lifecycle: retail "quit to main menu" path, absent from the UI catalogue.
    public const string SessionLoadingHold = "SESSION_LOADING_HOLD";
    // Internal round lifecycle: revokes the previous level incarnation at a lobby start.
    public const string SessionRoundBoundary = "SESSION_ROUND_BOUNDARY";
    public const string ReturnToFrontend = "RETURN_TO_FRONTEND";
    // Internal ItemSync: remove a pickup another participant collected, absent from the UI catalogue.
    public const string ItemSyncApply = "ITEM_SYNC_APPLY";
    // Internal ItemSync: spawn a Host-decided GCIP pickup or a remote player drop.
    public const string ItemSyncSpawn = "ITEM_SYNC_SPAWN";
    public const string MissionSyncConfigure = "MISSION_SYNC_CONFIG";
    public const string MissionSyncApply = "MISSION_SYNC_APPLY";

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            PopupMessage,
            SpawnCharacter,
            SpawnNetZCharacter,
            CharacterDirectMove,
            CharacterDirectStep,
            RemoteEquipmentProvision,
            CharacterActionTest,
            CharacterAmmoRefill,
            WorldRegistryProbe,
            WorldManifestLoad,
            WorldNetProbe,
            WorldInteractionStats,
            WorldCompositeInputStats,
            WorldWs3EmptyCallbackSet,
            PerformanceIsolationSet,
            WorldStateDiscoveryBaseline,
            WorldStateDiscoveryAfter,
            WorldStateDiscoveryStatus,
            ReturnToFrontend,
            SessionLoadingHold,
            SessionRoundBoundary,
            ItemSyncApply,
            ItemSyncSpawn,
            MissionSyncConfigure,
            MissionSyncApply
        },
        StringComparer.Ordinal);
}


public static class PerformanceIsolationNativeModules
{
    public const string PlayerSyncObserver = "player-sync-observer";
    public const string RemoteMovementRuntime = "remote-movement-runtime";
    public const string WorldSyncHooks = "worldsync-hooks";
    public const string CharacterLifecycle = "character-lifecycle";
    public const string EngineEvidence = "engine-evidence";
    public const string NativeDiagnostics = "native-diagnostics";
    public const string LocomotionFlight = "locomotion-flight";
    public const string GameThreadPumps = "game-thread-pumps";

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[]
        {
            PlayerSyncObserver, RemoteMovementRuntime, WorldSyncHooks,
            CharacterLifecycle, EngineEvidence, NativeDiagnostics,
            LocomotionFlight, GameThreadPumps
        },
        StringComparer.Ordinal);
}

public static class PopupMessageIcons
{
    public const string Alert = nameof(Alert);
    public const string Audiolog = nameof(Audiolog);
    public const string None = nameof(None);

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[] { Alert, Audiolog, None },
        StringComparer.Ordinal);
}

public static class PopupMessageSoundEvents
{
    public const string ObjectiveNew = "OBJECTIVE_NEW";

    public static readonly IReadOnlySet<string> All = new HashSet<string>(
        new[] { ObjectiveNew },
        StringComparer.Ordinal);
}

public sealed record NativeCapabilityState(
    string Name,
    string State,
    string Reason);

public sealed record TestModeConfigurePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    bool Enabled);

public sealed record CapabilityQueryPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity);

public sealed record CapabilityStatePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string ProfileId,
    string ResolutionMode,
    bool StaticValidated,
    bool RuntimeValidated,
    IReadOnlyList<NativeCapabilityState> Capabilities,
    IReadOnlyList<string> BlockedGates,
    string NextAction);

public sealed record CaptureStatePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    bool SpawnRequestF0Captured,
    bool SpawnRequest130Captured,
    bool ParticipantRegistryAvailable,
    int ParticipantCount,
    bool ContextPoolAvailable,
    bool FreeChannelAvailable,
    bool GameThreadValidated,
    int CaptureCatalogCount,
    string LastOperation,
    int GameThreadCandidateId = 0,
    int GameThreadSourceCount = 0,
    int EngineThreadId = 0,
    string EngineThreadSource = "Unknown",
    ulong FirstEngineCallbackPerformanceCounter = 0,
    ulong LastEngineCallbackPerformanceCounter = 0,
    int PendingCommandCount = 0,
    bool MainThreadCharacterProcessingHookInstalled = false,
    bool CharacterManagerUpdateHookInstalled = false,
    bool CharacterReadinessProducerHookInstalled = false,
    bool EngineHooksInstalled = false,
    string RemoteCharacterState = "None",
    uint RemoteCharacterSpawnKey = 0,
    ulong RemoteCharacterAddress = 0,
    ulong RemoteCharacterRecordAddress = 0,
    bool CharacterCreateHookInstalled = false,
    ulong RemoteCharacterCreationSerial = 0,
    uint RemoteCharacterRuntimeId = 0);

public static class RemoteCharacterStateNames
{
    public const string None = nameof(None);
    public const string Pending = nameof(Pending);
    public const string Ready = nameof(Ready);
    public const string Failed = nameof(Failed);

    public static IReadOnlySet<string> All { get; } = new HashSet<string>(
        new[] { None, Pending, Ready, Failed },
        StringComparer.Ordinal);
}

public sealed record DummyIdentityPayload(
    Guid PortableDummyId,
    ulong PeerId,
    Guid LauncherPlayerId,
    Guid PortableBindingId,
    string DisplayName);

public sealed record SpawnOffsetPayload(
    float X,
    float Y,
    float Z);

public sealed record DummyCreatePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    DummyIdentityPayload Identity,
    SpawnOffsetPayload SpawnOffset);

public sealed record DummyRemovePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId);

public sealed record DummyOperationResultPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    string State,
    string AchievedLevel,
    string StatusCode,
    string Message,
    bool RollbackPerformed,
    IReadOnlyList<string> CompletedSteps,
    IReadOnlyList<string> BlockedGates);

public sealed record DummyMovePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    string Command,
    float Amount,
    int DurationMilliseconds);

public sealed record DummyMoveResultPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    string StatusCode,
    string Message,
    string Method,
    bool Applied);

public sealed record DummyBindingStatePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    Guid PortableDummyId,
    bool ParticipantBound,
    bool CharacterPresent,
    bool CharacterRecordPresent,
    bool ChannelActive,
    bool DescriptorsPresent,
    bool ReplicationContextPresent,
    string AchievedLevel,
    string LastError);

public sealed record DiagnosticSnapshotRequestPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    bool Flush);

public sealed record DiagnosticSnapshotResultPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string RunId,
    string NativeLogPath,
    string ProfilePath,
    string CaptureDirectory,
    string DumpPath,
    IReadOnlyList<string> ArtifactPaths,
    string StatusCode,
    string Message);

public sealed record ExperimentalErrorPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string Code,
    string Message,
    IReadOnlyList<string> BlockedGates);

public sealed record LevelStatePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string LevelName,
    bool Transitioning,
    bool GameThreadValidated);

public sealed record GameCommandExecutePayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string Command,
    IReadOnlyDictionary<string, string> Arguments);

public sealed record GameCommandResultPayload(
    int SchemaVersion,
    Guid RequestId,
    int LevelGeneration,
    [property: JsonPropertyName("portableDummyId")] Guid RuntimeIdentity,
    string Command,
    string StatusCode,
    string Message,
    bool Executed);
