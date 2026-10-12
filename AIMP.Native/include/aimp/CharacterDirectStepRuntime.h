#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp {

class HookManager;

namespace character_direct_step_runtime {

// Resolved retail locomotion carrier observed after Host pathfinding/local
// steering but before ActNPCLocomotionCycle transforms it into Character-local
// motion. It is an intention, not a pose: no position or animation state is
// transported. The Client feeds the same world-space request back through the
// retail locomotion cycle so CATHODE owns turning, collision and animation.
struct NativeAIIntent final {
    float directionX{}, directionZ{}, intensity{};
    std::uint32_t moveType{0xFFFFFFFFU};
    bool moving{};
};
using NativeAIIntentObserver = void(*)(
    std::uintptr_t characterAddress,
    const NativeAIIntent& intent) noexcept;

struct RuntimeSnapshot {
    std::uintptr_t characterAddress{};
    std::uint32_t remainingFrames{};
    std::uint64_t activationAttempts{};
    std::uint64_t activationApplications{};
    std::uint64_t activationReuses{};
    std::uint64_t activationFailures{};
    std::uint64_t nativeStateLosses{};
    std::uint64_t nativeStateReacquireAttempts{};
    std::uint64_t nativeStateReacquireSuccesses{};
    std::uint64_t enterMoveTransitions{};
    std::uint64_t updateMoveTransitions{};
    std::uint64_t exitMoveTransitions{};
    std::uint64_t idleHoldApplications{};
    std::uint64_t enginePumpApplications{};
    std::uint64_t aiActivationSuppressions{};
    std::uint64_t aiIdleSuppressions{};
    std::uint64_t nativeIdleApplications{};
    std::uint64_t followerApplications{};
    std::uint64_t effectiveMoveTypeApplications{};
    std::uint64_t hookHits{};
    std::uint64_t finalSubmissions{};
    std::uint64_t requestApplications{};
    std::uint64_t transformApplications{};
    std::uint64_t teleportApplications{};
    std::uint64_t hardResyncAttempts{};
    std::uint64_t hardResyncApplications{};
    std::uint64_t physicalApplications{};
    std::uint64_t stopApplications{};
    std::uint64_t protectedFailures{};
    std::uintptr_t lastCharacter{};
    std::uintptr_t lastStateAddress{};
    std::uint8_t nativeCycleState{};
    bool nativeStateActive{};
    bool moveActive{};
    bool armed{};
};

// Remote-player replication owns the locomotion state machine for bound
// Characters. Network commands only publish desired motion/transform data; the
// actual native Move/Idle transitions are performed on the engine thread. AI
// move activations are suppressed for bound remote Characters while the native
// animation/locomotion cycle itself remains untouched and continues to tick.
bool Install(
    std::uintptr_t moduleBase,
    HookManager& hooks,
    std::string& error);

[[nodiscard]] bool Installed() noexcept;

// Passive engine-thread tap used by AISync Host authority. The observer sees
// the retail-resolved steering request even when no remote-movement slot owns
// the Character. Passing nullptr detaches it.
void SetNativeAIIntentObserver(NativeAIIntentObserver observer) noexcept;

// Authoritative NPC intent replay. Unlike ArmWorldReplication these functions
// carry no network transform and therefore never chase/snap toward a position.
// A short lease makes a lost Host update settle to retail Idle rather than
// continuing the last direction forever.
bool ArmAIIntentReplication(
    std::uintptr_t characterAddress,
    float directionX,
    float directionZ,
    float intensity,
    std::uint32_t moveType,
    std::string& error) noexcept;

bool HoldAIIntentIdle(
    std::uintptr_t characterAddress,
    std::uint32_t moveType,
    std::string& error) noexcept;

enum class AIExecutionReplayResult : std::uint8_t {
    NotRequired = 0U,
    Applied = 1U,
    Unsupported = 2U,
    Failed = 3U,
};

// Replays a Host-selected Character execution transition through the same
// retail transition entry point. Only transitions whose ABI and destination
// cycle identity are certified are admitted (including the
// ignored stack word of the native 3B stop transition); unknown states are
// deliberately left untouched rather than fabricating a native C++ object.
AIExecutionReplayResult ReplayAIExecutionTransition(
    std::uintptr_t characterAddress,
    std::uint32_t cycleTypeRva,
    std::uint32_t cycleState,
    std::string& error) noexcept;

// Drops only AIMP's persistent intent slot. Unlike Stop(), this deliberately
// does not force Idle or submit a zero movement request, so a just-replayed
// retail transition remains fully owned by CATHODE.
bool ReleaseAIIntentOwnership(std::uintptr_t characterAddress) noexcept;

// Called from CharacterManagerUpdate on the engine thread. PRE owns normal
// follower preparation and native transitions. POST is a reconciliation pass
// that only performs work when state changed during the retail manager tick.
enum class EnginePumpPhase : std::uint8_t {
    PreCharacterManager = 0U,
    PostCharacterManager = 1U,
};

void PumpOnEngineThread(EnginePumpPhase phase) noexcept;

bool ArmDirection(
    std::uintptr_t characterAddress,
    float directionX,
    float directionZ,
    float intensity,
    std::uint32_t frameCount,
    std::string& error) noexcept;

bool ArmWorldDirection(
    std::uintptr_t characterAddress,
    float directionX,
    float directionZ,
    float intensity,
    std::uint32_t frameCount,
    std::string& error) noexcept;

bool ArmWorldReplication(
    std::uintptr_t characterAddress,
    float directionX,
    float directionZ,
    float intensity,
    float networkX,
    float networkY,
    float networkZ,
    float replicationYaw,
    std::uint32_t moveType,
    std::uint32_t frameCount,
    std::string& error) noexcept;

// Keep the remote Character bound while the network player is idle. The
// engine-thread controller settles any residual position error through native
// locomotion, enters Idle once, and the +257900 detour prevents CIVILIAN AI
// from starting a return-to-spawn Move while the slot remains owned.
bool HoldWorldReplicationIdle(
    std::uintptr_t characterAddress,
    float networkX,
    float networkY,
    float networkZ,
    float replicationYaw,
    std::uint32_t moveType,
    std::string& error) noexcept;

// Engine thread only, after Arm/HoldWorldReplication succeeds. Selects the
// NPC follower limits for this owned slot; release/reallocation clears them.
// AISync already owns placement boundaries: initial follower binding converges
// from the current native pose without an implicit placement teleport.
// PlayerSync and manual movement retain the default profile.
bool UseAIReplicationProfile(std::uintptr_t characterAddress) noexcept;

bool ApplyReplicatedYaw(
    std::uintptr_t characterAddress,
    float yaw,
    std::string& error) noexcept;

// Engine thread only: leave a completed authoritative attack via the retail
// Idle transition/destructor. Never touches a victim's death cycle or health.
bool FinishReplicatedInteraction(std::uintptr_t characterAddress) noexcept;

bool Arm(
    std::uintptr_t characterAddress,
    float deltaX,
    float deltaY,
    float deltaZ,
    float deltaYaw,
    std::uint32_t frameCount,
    std::string& error) noexcept;

void Stop() noexcept;
[[nodiscard]] bool Stop(std::uintptr_t characterAddress) noexcept;
[[nodiscard]] std::uint32_t RemainingFrames() noexcept;
[[nodiscard]] std::uint32_t RemainingFrames(std::uintptr_t characterAddress) noexcept;
[[nodiscard]] RuntimeSnapshot Snapshot() noexcept;
[[nodiscard]] RuntimeSnapshot Snapshot(std::uintptr_t characterAddress) noexcept;

[[nodiscard]] constexpr std::size_t MaximumConcurrentTargets() noexcept {
    // Remote players plus AISync's bounded set of NPC Characters can be
    // active together. Slots are fixed-size and never allocate in a detour.
    return 40U;
}

struct NativeMovementRvaSet {
    std::uintptr_t activateLocomotionCycle{};
    std::uintptr_t enterIdleCycle{};
    std::uintptr_t movementActionPostGaitSelection{};
    std::uintptr_t npcLocomotionCycleUpdate{};
    std::uintptr_t npcLocomotionVtable{};
    std::uintptr_t npcLocomotionPreTransformControl{};
    std::uintptr_t npcLocomotionPostSubmit{};
    std::uintptr_t submitLocomotion{};
    std::uintptr_t teleportCharacter{};
};

[[nodiscard]] constexpr NativeMovementRvaSet NativeMovementRvas() noexcept {
    return NativeMovementRvaSet{
        0x00257900U,
        0x0025BDD0U,
        0x0005B874U,
        0x0026D4B0U,
        0x00EABF80U,
        0x0026D4FEU,
        0x0026D5BAU,
        0x0028D6D0U,
        0x003ECCA0U};
}

struct CharacterRegistryLayout {
    std::uintptr_t table{};
    std::size_t stride{};
    std::size_t capacity{};
    std::uintptr_t character{};
    std::uintptr_t physicalBody{};
    std::uintptr_t secondaryBody{};
    std::uintptr_t active{};
};

[[nodiscard]] constexpr CharacterRegistryLayout NativeCharacterRegistry() noexcept {
    return CharacterRegistryLayout{
        0x01307A48U,
        0x00B0U,
        18U,
        0x0000U,
        0x000CU,
        0x0044U,
        0x00A8U};
}

[[nodiscard]] constexpr std::uintptr_t ReplicatedYawOffset() noexcept {
    return 0x0174U;
}

[[nodiscard]] constexpr std::size_t NativeLocomotionFlagWidth() noexcept {
    return sizeof(std::uint8_t);
}

struct NativeCycleControlOffsets {
    std::uintptr_t magnitude{};
    std::uintptr_t worldHeading{};
    std::uintptr_t intensity{};
};

[[nodiscard]] constexpr NativeCycleControlOffsets NativeCycleOffsets() noexcept {
    return NativeCycleControlOffsets{0x004CU, 0x0050U, 0x005CU};
}

struct NativeCycleControlPlan {
    float magnitude{};
    float worldHeading{};
    float intensity{};
};

// ActNPCLocomotionCycle converts these source fields to a Character-local X/Z
// vector before calling +28D6D0 itself. This legacy builder remains useful for
// direct diagnostic commands that intentionally provide either world or local
// headings. Network replication uses the orientation-aware builder below.
[[nodiscard]] bool TryBuildNativeCycleControlPlan(
    float directionX,
    float directionZ,
    float intensity,
    bool worldSpace,
    float characterYaw,
    NativeCycleControlPlan& plan) noexcept;

// Retail ActNPCLocomotionCycle subtracts an orientation value fetched from its
// own movement object before constructing cycle+68/cycle+64. To reproduce a
// remote player's motion exactly, convert the source world vector into the
// source Character's local frame, then bias the cycle heading by the receiver's
// native orientation so the retail subtraction reconstructs that same local
// vector. This avoids hard-coded +/-90 or 180 degree corrections.
[[nodiscard]] bool TryBuildOrientationAwareNativeCycleControlPlan(
    float worldDirectionX,
    float worldDirectionZ,
    float intensity,
    float replicatedYaw,
    float nativeOrientation,
    NativeCycleControlPlan& plan) noexcept;

struct PhysicalDeltaOffsets {
    std::uintptr_t x{};
    std::uintptr_t y{};
    std::uintptr_t z{};
    std::uintptr_t yaw{};
};

[[nodiscard]] constexpr PhysicalDeltaOffsets PhysicalOffsets() noexcept {
    return PhysicalDeltaOffsets{0x1010U, 0x1014U, 0x1018U, 0x1020U};
}

} // namespace character_direct_step_runtime
} // namespace aimp
