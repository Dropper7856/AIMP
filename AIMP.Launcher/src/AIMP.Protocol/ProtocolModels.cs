using System.Text.Json.Serialization;

namespace AIMP.Protocol;

public sealed record CompatibilityInfo(
    ushort ProtocolVersion,
    string LauncherVersion,
    string DllVersion,
    string AiSha256,
    string ProfileId,
    string CompatibilityHash);

public sealed record HelloPayload(
    ushort ProtocolVersion,
    string DllVersion,
    int ProcessId,
    bool ObservationOnly,
    string Nonce,
    bool AiSyncV1Available = false,
    bool UiHeapSynchronized = false);

public sealed record HelloAckPayload(
    ushort ProtocolVersion,
    string LauncherVersion,
    int ProcessId,
    string Nonce);

public sealed record BuildInfoPayload(
    string AiSha256,
    string ProfileId,
    bool ProfileValidated);

public sealed record NetZReadyPayload(
    string Mode,
    bool NativeReplicationReady,
    IReadOnlyList<string> BlockedGates);

public sealed record SessionConfigPayload(
    Guid SessionId,
    ulong PeerId,
    Guid LauncherPlayerId,
    string DisplayName,
    bool IsHost,
    string HostAddress,
    int HostPort,
    int LevelGeneration,
    bool NativeReplicationEnabled);

// Local Launcher <-> injected DLL lifecycle. This is deliberately distinct
// from SessionConfig: bootstrap IPC proves process compatibility, while this
// payload binds the real multiplayer authority and opens/closes the WS3 world
// bus only after the Launcher has a compatible live WorldContext.
public sealed record WorldSyncLifecyclePayload(
    Guid SessionId,
    ulong PeerId,
    bool IsHost,
    int LevelGeneration,
    bool AuthorityBound,
    bool WorldBusActive,
    string Reason);

public sealed record ErrorPayload(string Code, string Message);

public enum WorldTransactionRecoveryStatusCode : byte
{
    Complete = 0,
    Unavailable = 1,
    ContextRejected = 2
}

// Control-plane only. Gameplay mutations remain WorldNetworkMutation/ROOT v10.
public sealed record WorldTransactionRecoveryRequest(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    ulong LastCommittedSequence);

public sealed record WorldTransactionRecoveryStatus(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    WorldTransactionRecoveryStatusCode Status,
    ulong RequestedAfterSequence,
    ulong HostThroughSequence,
    string Reason);

public sealed record LobbyJoinPayload(
    Guid LauncherPlayerId,
    string DisplayName,
    CompatibilityInfo Compatibility,
    long ConnectionNonce);

public sealed record LobbyJoinAcceptedPayload(
    Guid SessionId,
    ulong PeerId,
    int Capacity,
    bool OpaqueRelayEnabled,
    long ConnectionGeneration);

public sealed record LobbyReadyPayload(bool Ready);

public sealed record GameplayReadyPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration);

// Client -> Host: the sender's own THE_PLAYER reached its terminal death
// boundary during this launch generation.
public sealed record PlayerDeathPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration);

// Host -> Clients: every participant of the generation is dead. The Host is
// already back in Lobby; peers show the end message, then return to frontend.
public sealed record RoundEndedPayload(
    Guid SessionId,
    int LevelGeneration,
    string Reason,
    int ReturnDelayMilliseconds);

// Client -> Host: the sender's own player collected the world pickup EntityId
// (32-hex WorldEntityId from the level's WorldManifest).
public sealed record ItemPickupRequestPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    string EntityId);

// Host -> Clients: the first collector of EntityId in this generation. Every
// other participant removes the pickup from its own world.
public sealed record ItemCollectedPayload(
    Guid SessionId,
    int LevelGeneration,
    string EntityId,
    ulong CollectorPeerId);

// ItemSync spawn authority. Kind "owner": a GCIP_WorldPickup spawn result for
// OwnerKey (CATHODE runtime key, 16 hex), Spawn=false when nothing spawns.
// Kind "drop": a player-dropped pickup with a synthetic OwnerKey. Transform is
// the 3x4 row-major pickup transform (translation at [3], [7], [11]).
// Proposal (Client -> Host): PeerId is the sender. Decision (Host -> Clients):
// PeerId is the participant whose proposal won.
public sealed record ItemSpawnPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    string Kind,
    string OwnerKey,
    bool Spawn,
    string Path,
    int Quantity,
    float[] Transform);

public sealed record PlayerSpawnPositionPayload(
    ulong PeerId,
    float X,
    float Y,
    float Z);

public sealed record SpawnPositionRequestPayload(
    Guid SessionId,
    Guid RequestId,
    ulong HostPeerId,
    int LevelGeneration);

public sealed record SpawnPositionResponsePayload(
    Guid SessionId,
    Guid RequestId,
    ulong PeerId,
    int LevelGeneration,
    float X,
    float Y,
    float Z);

public sealed record SpawnRemotePlayersPayload(
    Guid SessionId,
    Guid RequestId,
    ulong HostPeerId,
    int LevelGeneration,
    IReadOnlyList<PlayerSpawnPositionPayload> Players);

public enum PlayerMoveType
{
    SlowWalk = 0,
    Walk = 1,
    FastWalk = 2,
    Run = 3
}

public sealed record PlayerPostureSnapshot(
    [property: JsonRequired] bool Crouched,
    [property: JsonRequired] bool DeepCrouched,
    [property: JsonRequired] bool InVent,
    [property: JsonRequired] uint VentIdA,
    [property: JsonRequired] uint VentIdB,
    [property: JsonRequired] ulong TraversalRevision,
    [property: JsonRequired] ushort TraversalIndex,
    [property: JsonRequired] byte TraversalType,
    [property: JsonRequired] uint TraversalOwner,
    [property: JsonRequired] bool TraversalActive);

// Effective emitted beam state; retail battery/reload logic remains authoritative.
public sealed record PlayerTorchSnapshot([property: JsonRequired] bool Enabled);

public sealed record PlayerPositionSnapshotPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    float X,
    float Y,
    float Z,
    float Yaw,
    PlayerMoveType MoveType,
    float NativeMoveSpeed,
    PlayerPostureSnapshot? Posture = null,
    PlayerTorchSnapshot? Torch = null,
    // Source-owner allocated, monotone within this session/peer. Zero means
    // unavailable. Never a runtime ID or permission to enter a native pair.
    uint CharacterIncarnation = 0);

public enum PlayerActionKind
{
    Aim = 0,
    Primary = 1,
    Reload = 2
}

public sealed record NativePlayerEquipmentStatePayload(
    int SchemaVersion,
    ulong Revision,
    bool Valid,
    int CurrentEquipmentIndex,
    int PreviousEquipmentIndex,
    bool EquipmentActive,
    int PresentationState,
    bool AimHeld,
    bool PrimaryHeld);

public sealed record NativePlayerLifecycleStatePayload(
    int SchemaVersion,
    ulong DeathSequence,
    bool LocalPlayerDead);

public sealed record NativeItemSyncPickupPayload(
    int SchemaVersion,
    ulong Sequence,
    string EntityId);

public sealed record NativeItemSyncSpawnPayload(
    int SchemaVersion,
    ulong Sequence,
    string Kind,
    string OwnerKey,
    bool Spawn,
    string Path,
    int Quantity,
    float[] Transform);

public sealed record NativePlayerActionEventPayload(
    int SchemaVersion,
    ulong EventSequence,
    PlayerActionKind Action,
    bool Active,
    int CurrentEquipmentIndex,
    int PresentationState);

public sealed record PlayerEquipmentSnapshotPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    int CurrentEquipmentIndex,
    int PreviousEquipmentIndex,
    bool EquipmentActive,
    int PresentationState,
    bool AimHeld,
    bool PrimaryHeld);

public sealed record PlayerActionEventPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    PlayerActionKind Action,
    bool Active,
    int CurrentEquipmentIndex,
    int PresentationState);

public sealed record LobbyStartPayload(
    Guid SessionId,
    long StartAtUnixMilliseconds,
    string Level,
    int LevelGeneration,
    bool AlienEnabled = true);

public sealed record LobbyPlayer(
    ulong PeerId,
    Guid LauncherPlayerId,
    string DisplayName,
    bool IsHost,
    bool Ready);

public sealed record LobbySnapshotPayload(
    Guid SessionId,
    string ServerName,
    string Level,
    string State,
    int Capacity,
    int LevelGeneration,
    IReadOnlyList<LobbyPlayer> Players,
    Guid PreparationToken = default,
    IReadOnlyList<LobbyPreparationPayload>? Preparations = null,
    bool WorldPreparationRequired = false,
    bool AlienEnabled = true);

public sealed record DiscoveryAnnouncement(
    ushort ProtocolVersion,
    Guid SessionId,
    string ServerName,
    string HostName,
    int PlayerCount,
    int Capacity,
    string Level,
    string State,
    int TcpPort,
    string CompatibilityHash);

public sealed record DiscoveredSession(
    DiscoveryAnnouncement Announcement,
    string Address,
    DateTimeOffset LastSeenUtc);


// A report proves only portable compiled content, never live native readiness.
public sealed record LobbyPreparationPayload(
    Guid SessionId,
    ulong PeerId,
    string Level,
    Guid PreparationToken,
    WorldManifestIdentity? Manifest,
    string ManifestSha256,
    bool Ready,
    string Error);

// Sent only after world recovery, native binding and remote players are ready.
public sealed record SessionPreparedPayload(Guid SessionId, ulong PeerId, int LevelGeneration, string MissionSha256 = "");

// Host-only permission to expose gameplay for this exact start generation.
public sealed record SessionReleasePayload(Guid SessionId, int LevelGeneration);
