namespace AIMP.Protocol;

public enum MessageType : ushort
{
    Hello = 1,
    HelloAck = 2,
    BuildInfo = 3,
    SessionConfig = 4,
    NetZReady = 5,
    LevelChanged = 6,
    PeerJoin = 7,
    PeerLeave = 8,
    PeerBind = 9,
    OutboundNetZFrame = 10,
    InboundNetZFrame = 11,
    ResyncRequest = 12,
    Error = 13,
    Heartbeat = 14,
    Shutdown = 15,
    WorldIntent = 16,
    WorldMutation = 17,
    WorldMutationResult = 18,
    WorldIntentRejected = 19,
    // Launcher <-> DLL control plane only. These values never travel on the
    // multiplayer peer transport despite sharing the common frame enum.
    WorldSyncLifecycle = 20,
    WorldIntentAbandoned = 21,
    PlayerSyncState = 22,
    PlayerSyncAction = 23,
    AISyncConfigure = 24,
    AISyncState = 25,
    AISyncImpact = 26,
    PlayerLifecycleState = 27, // DLL -> launcher only: local THE_PLAYER death boundary.
    ItemSyncPickup = 28, // DLL -> launcher only: the local player collected a world pickup.

    LobbyJoin = 100,
    LobbyJoinAccepted = 101,
    LobbySnapshot = 102,
    LobbyReady = 103,
    LobbyStart = 104,
    LobbyLeave = 105,
    GameplayReady = 106,
    SpawnRemotePlayers = 107,
    SpawnPositionRequest = 108,
    SpawnPositionResponse = 109,
    PlayerPositionSnapshot = 110,
    WorldContext = 111,
    WorldNetworkIntent = 112,
    WorldNetworkMutation = 113,
    WorldNetworkMutationAck = 114,
    WorldNetworkIntentRejected = 115,
    WorldTransactionRecoveryRequest = 116,
    WorldTransactionRecoveryStatus = 117,
    PlayerEquipmentSnapshot = 118,
    PlayerActionEvent = 119,
    AISyncNetworkState = 120, // Host -> Clients only; never relayed from a Client.
    AISyncNetworkImpact = 121, // Client -> Host only; Host applies consequence natively.
    PlayerDeath = 122, // Client -> Host only; the Host owns round termination.
    RoundEnded = 123, // Host -> Clients only; the session is back in Lobby.
    ItemPickupRequest = 124, // Client -> Host only; the Host arbitrates the first collector.
    ItemCollected = 125, // Host -> Clients only; the pickup is gone for every participant.

    TestModeConfigure = 200,
    CapabilityQuery = 201,
    CapabilityState = 202,
    CaptureState = 203,
    DummyCreate = 204,
    DummyCreateResult = 205,
    DummyRemove = 206,
    DummyRemoveResult = 207,
    DummyMove = 208,
    DummyMoveResult = 209,
    DummyBindingState = 210,
    DiagnosticSnapshotRequest = 211,
    DiagnosticSnapshotResult = 212,
    ExperimentalError = 213,
    LevelState = 214,
    GameCommandExecute = 215,
    GameCommandResult = 216,
}
