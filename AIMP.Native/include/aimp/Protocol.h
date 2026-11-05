#pragma once

#include "aimp/Common.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aimp::protocol {

// Bytes on the wire are ASCII "AIMP".
inline constexpr std::uint32_t kMagic = 0x504D4941U;
inline constexpr std::size_t kWireHeaderBytes = 22U;

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    BuildInfo = 3,
    SessionConfig = 4,
    NetzReady = 5,
    LevelChanged = 6,
    PeerJoin = 7,
    PeerLeave = 8,
    PeerBind = 9,
    OutboundNetzFrame = 10,
    InboundNetzFrame = 11,
    ResyncRequest = 12,
    Error = 13,
    Heartbeat = 14,
    Shutdown = 15,
    WorldIntent = 16,
    WorldMutation = 17,
    WorldMutationResult = 18,
    WorldIntentRejected = 19, // Local IPC disposition; never an authoritative replay.
    WorldSyncLifecycle = 20, // Local Launcher<->DLL authority/bus lifecycle only.
    WorldIntentAbandoned = 21, // Local pre-bus disposition; never a Host rejection.
    PlayerSyncState = 22, // Native -> Launcher local player-state observation only.
    AISyncConfigure = 24, // Local IPC authority/generation configuration.
    AISyncState = 25, // Native <-> Launcher generic NPC authority snapshots.
    AISyncImpact = 26, // Client-native weapon impact -> Host-native AI consequence.
    PlayerSyncAction = 23, // Native -> Launcher ordered Aim/Primary/Reload edge.
    PlayerLifecycleState = 27, // Native -> Launcher local THE_PLAYER death boundary.
    ItemSyncPickup = 28, // Native -> Launcher: the local player collected a world pickup.
    ItemSyncSpawn = 29, // Native -> Launcher: GCIP spawn proposal or local player drop.
    VoiceConfigure = 30,
    VoicePcmFrame = 31,
    ThrowableConfigure = 32,
    ThrowableEvent = 33,
    AISyncTransactionObservation = 34, // Native -> Launcher observation only, never replay input.
    AISyncExecutionBinding = 35,
    AISyncExecutionBatch = 36,
    AISyncExecutionReceipt = 37,
    AISyncExecutionRetry = 38,
    AISyncHostBinding = 39, // Launcher -> Host DLL typed producer scope.
    AISyncHostBatch = 40, // Host DLL -> Launcher complete executable operations.
    AISyncHostControl = 41, // Host DLL -> Launcher producer readiness/recovery.
    AISyncHostRoute = 42, // Host DLL -> Launcher solved route for an admitted command.
    AISyncClientRoute = 43, // Launcher -> Client DLL authenticated command route.
    CharacterIncarnationState = 44, // DLL -> Launcher local THE_PLAYER ticket only.
    PlayerHidingState = 47, // DLL -> Launcher local presentation observation only.
    AISyncHostState = 45, // Event carrier in the authoritative execution outbox.
    AISyncClientState = 46, // Admitted through the execution mailbox.
    MissionSignalProduced = 134,
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
};

struct Frame {
    MessageType type{MessageType::Error};
    std::uint16_t flags{};
    std::uint64_t sequence{};
    std::vector<std::byte> payload;
};

enum class DecodeResult : std::uint8_t {
    Complete,
    NeedMoreData,
    InvalidMagic,
    UnsupportedVersion,
    PayloadTooLarge,
    InvalidType,
    InvalidFlags,
};

bool IsKnownMessageType(std::uint16_t raw) noexcept;

bool EncodeFrame(
    MessageType type,
    std::uint16_t flags,
    std::uint64_t sequence,
    std::span<const std::byte> payload,
    std::vector<std::byte>& output);

DecodeResult DecodeFrame(
    std::span<const std::byte> input,
    Frame& output,
    std::size_t& consumed);

// Parses only the fixed header. payloadLength is valid only for Complete.
DecodeResult DecodeHeader(
    std::span<const std::byte> input,
    MessageType& type,
    std::uint16_t& flags,
    std::uint32_t& payloadLength,
    std::uint64_t& sequence);

} // namespace aimp::protocol
