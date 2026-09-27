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
    string Nonce);

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
    float NativeMoveSpeed);

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
    int LevelGeneration);

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
    IReadOnlyList<LobbyPlayer> Players);

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
