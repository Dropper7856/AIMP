#pragma once

#include "aimp/Common.h"
#include "aimp/WorldReplicationTypes.h"

#include <cstdint>
#include <span>
#include <string>

namespace aimp {

class HookManager;
class SignatureResolver;
class WorldEntityLease;
class WorldEntityRegistry;

namespace world_animated {

inline constexpr std::uint32_t kOperationDoorAction = 0x000C0701U;
inline constexpr std::uint32_t kOperationAnimatedInputEdge = 0x000C0702U;
// Certified retail DoorNative semantic inputs. Patch 5.11 transports only the
// stable desired lock-state operation; no native pointer/context crosses IPC.
inline constexpr std::uint32_t kDoorRequestLockInput = 0x5D7280CFU;
inline constexpr std::uint32_t kDoorRequestUnlockInput = 0x440FBB74U;
[[nodiscard]] constexpr bool IsDoorLockStateInput(const std::uint32_t input) noexcept {
    return input == kDoorRequestLockInput || input == kDoorRequestUnlockInput;
}
inline constexpr std::uint8_t kPayloadVersion = 1U;
inline constexpr std::size_t kDoorActionPayloadBytes = 16U;
inline constexpr std::size_t kAnimatedInputEdgePayloadBytes = 48U;

enum class DoorAction : std::uint8_t {
    Open = 1,
    Close = 2,
    Lock = 3,
    Unlock = 4,
    ForceOpen = 5,
    ForceClose = 6,
};

enum class AnimatedTargetCategory : std::uint8_t {
    PlayEnvironmentAnimation = 1,
    AnimatedModelAttachmentNode = 2,
};

struct DoorActionPayload final {
    DoorAction action{DoorAction::Open};
    std::uint64_t observationId{};
};

struct AnimatedInputEdge final {
    AnimatedTargetCategory targetCategory{AnimatedTargetCategory::PlayEnvironmentAnimation};
    std::uint32_t schedulerParameterShortGuid{};
    std::uint32_t targetInputShortGuid{};
    std::uint64_t observationId{};
    WorldEntityId sourceEntity{};
    float relativeDelaySeconds{};
};

enum class ExecuteStatus : std::uint8_t {
    Queued = 0,
    SuppressedLocalMatch,
    InvalidPayload,
    RuntimeUnavailable,
    NativeFault,
};

struct ExecuteResult final {
    ExecuteStatus status{ExecuteStatus::RuntimeUnavailable};
    bool queued{};
};

struct RuntimeDiagnostics final {
    bool initialized{};
    bool doorHooksInstalled{};
    bool doorUnlockProbeInstalled{};
    bool doorSemanticProbeInstalled{};
    std::uint64_t version{};
    std::uint64_t initializationCount{};
    std::uint64_t shutdownCount{};
    std::uint64_t doorOutputCalls{};
    std::uint64_t doorInputCalls{};
    std::uint64_t doorUnlockProbeCalls{};
    std::uint64_t doorUnlockProbeSuccess{};
    std::uint64_t doorUnlockProbeResolved{};
    std::uint64_t doorUnlockProbeUnresolved{};
    std::uint64_t doorUnlockProbeLastEntityHigh{};
    std::uint64_t doorUnlockProbeLastEntityLow{};
    std::uint64_t doorRequestLockProbeCalls{};
    std::uint64_t doorRequestLockProbeSuccess{};
    std::uint64_t doorRequestLockProbeResolved{};
    std::uint64_t doorForceOpenProbeCalls{};
    std::uint64_t doorForceOpenProbeSuccess{};
    std::uint64_t doorForceOpenProbeResolved{};
    std::uint64_t doorForceCloseProbeCalls{};
    std::uint64_t doorForceCloseProbeSuccess{};
    std::uint64_t doorForceCloseProbeResolved{};
    std::uint64_t doorSemanticProbeLastAction{};
    std::uint64_t doorSemanticProbeLastEntityHigh{};
    std::uint64_t doorSemanticProbeLastEntityLow{};
    std::uint64_t doorLockStateSourceResolved{};
    std::uint64_t doorLockStateSourceUnresolved{};
    std::uint64_t doorLockStateSelfSourceRejected{};
    std::uint64_t doorLockStateLastSourceHigh{};
    std::uint64_t doorLockStateLastSourceLow{};
    std::uint64_t doorObservedActions{};
    std::uint64_t doorUnresolved{};
    std::uint64_t doorRemoteSuppressed{};
    std::uint64_t doorOriginEchoSuppressed{};
    std::uint64_t doorTransitionRootAttempts{};
    std::uint64_t doorTransitionRootSubmitted{};
    std::uint64_t doorTransitionRootRetailOnly{};
    std::uint64_t doorTransitionRootAwaitAuthority{};
    std::uint64_t doorTransitionRootAuthoritySuppressed{};
    std::uint64_t animatedObservedEdges{};
    std::uint64_t animatedSubmittedIntents{};
    std::uint64_t animatedOriginMatchesSuppressed{};
    std::uint64_t animatedTargetClassMismatch{};
    std::uint64_t executeAttempts{};
    std::uint64_t queuedRemoteActions{};
    std::uint64_t queuedRemoteAnimatedEdges{};
    std::uint64_t invalidPayloads{};
    std::uint64_t runtimeUnavailable{};
    std::uint64_t nativeFaults{};
    std::uint64_t activeDoorDetours{};
};

[[nodiscard]] constexpr ModuleReport Report() noexcept {
    return {
        "WorldSync3.SemanticWorldAdapters",
        ModuleStatus::Implemented,
        "Legacy semantic-adapter codecs remain available for compatibility diagnostics. Patch 5.3.3 keeps Door started_opening/started_closing hooks dormant after the 5.3.2 synthetic callbackless Door self-edge caused a client-crash regression. Patch 5.11.1 preserves the proven RequestLock/RequestUnlock Client valve but carries the real stable CATHODE source context and rejects Door self-source replay; ForceOpen/ForceClose remain passive probes."};
}

[[nodiscard]] bool EncodeDoorActionPayload(
    const DoorActionPayload& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeDoorActionPayload(
    std::span<const std::byte> payload,
    DoorActionPayload& output) noexcept;
[[nodiscard]] bool EncodeAnimatedInputEdgePayload(
    const AnimatedInputEdge& value,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeAnimatedInputEdgePayload(
    std::span<const std::byte> payload,
    AnimatedInputEdge& output) noexcept;

bool Initialize(
    const SignatureResolver& resolver,
    HookManager& hooks,
    WorldEntityRegistry& registry,
    std::string& error) noexcept;
// Patch 5.3.3 compatibility entry point. Door transition capture is intentionally
// disabled fail-closed: 5.3.2 proved that synthesizing request_open/request_close
// from started_opening/started_closing as a callbackless Door self-edge is not a
// safe retail carrier. The function remains only to avoid ABI/source churn.
bool ArmDoorTransitionCapture(std::string& error) noexcept;
// Patch 5.5 compatibility entry point. Patch 5.6 expanded the probe to the
// certified RequestLock/RequestUnlock/ForceOpen/ForceClose DoorNative inputs.
// Patch 5.11 keeps retail-first behavior and promotes only Lock/Unlock to a
// stable Client-authored WS3 state root; ForceOpen/ForceClose remain probe-only.
bool ArmDoorUnlockProbe(std::string& error) noexcept;
bool ArmDoorSemanticInputProbe(std::string& error) noexcept;
[[nodiscard]] bool Initialized() noexcept;
[[nodiscard]] RuntimeDiagnostics DiagnosticsSnapshot() noexcept;

// Used by the already-installed CathodeQueueInput detour. This classification
// is constructor/vtable evidence-backed and performs no global scan.
[[nodiscard]] AnimatedTargetCategory ClassifyAnimatedTarget(
    const void* executionObject) noexcept;

// Called only after the retail scheduler has already accepted the local edge.
// The target entity travels in WorldIntent::Entity; source identity stays in
// the payload so the remote scheduler receives the same source shared-ref.
[[nodiscard]] bool ObserveAnimatedInputEdge(
    AnimatedTargetCategory targetCategory,
    std::uint32_t schedulerParameterShortGuid,
    std::uint32_t targetInputShortGuid,
    std::uint64_t observationId,
    const WorldEntityId& sourceEntity,
    const WorldEntityId& targetEntity,
    float relativeDelaySeconds) noexcept;

[[nodiscard]] std::uint64_t NextObservationId() noexcept;

// Suppresses both the origin reflection and an equivalent host-authored edge
// that races a client's optimistic local CATHODE execution.
[[nodiscard]] bool ConsumeLocalAnimatedMatch(
    const AnimatedInputEdge& edge,
    const WorldEntityId& targetEntity) noexcept;
[[nodiscard]] bool ConsumeLocalDoorObservation(std::uint64_t observationId) noexcept;

[[nodiscard]] ExecuteResult ExecuteDoorAction(
    const DoorActionPayload& payload,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target) noexcept;
// Patch 5.11.1 native WS3 DoorLockState replay. Only RequestLock/RequestUnlock
// are accepted. The receiver reconstructs the real stable source -> Door edge;
// a synthetic Door -> Door self-edge is never permitted.
[[nodiscard]] ExecuteResult ExecuteDoorLockState(
    std::uint32_t semanticInput,
    const WorldEntityId& sourceEntity,
    const WorldEntityLease& source,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target) noexcept;
[[nodiscard]] ExecuteResult ExecuteAnimatedInputEdge(
    const AnimatedInputEdge& edge,
    const WorldEntityId& targetEntity,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept;

void ResetForGeneration() noexcept;
void BeginLogicalShutdown() noexcept;

} // namespace world_animated
} // namespace aimp
