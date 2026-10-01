#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {
class HookManager;
class LevelGenerationTracker;
class GameThreadDispatcher;
class Diagnostics;

namespace ai_sync {
// Host decisions are NOT WorldMutation/PlayerSync messages. AISync owns every
// retail NPC Character at the Character/behaviour-runtime boundary. Like
// WorldSync, the wire carries stable identities plus high-level resolved
// carriers only. Animation clips/frames, skeleton state, audio, particles and
// downstream CATHODE consequences are deliberately absent: retail systems
// regenerate them locally after the carrier is applied.
// Process pointers and species-specific state never cross the network.
enum class Role : std::uint32_t { Retail, Host, Client, FrozenClient };

enum StateFlag : std::uint32_t {
    PoseValid = 1U << 0U,
    Moving = 1U << 1U,
    ActionValid = 1U << 2U,
    InteractionValid = 1U << 3U,
    RoleTaskValid = 1U << 4U,
};

enum LifecycleFlag : std::uint32_t {
    LifecycleSpatialDiscontinuity = 1U << 0U,
    LifecycleTraversalAction = 1U << 1U,
    // Persistent Host lifecycle state. Withdrawn maps the Alien retail
    // withdraw controller (Character+34F0 -> +18); State2 preserves the
    // controller's second non-zero mode. HostDormant covers backstage periods
    // where the BehaviourManager itself no longer ticks.
    LifecycleWithdrawn = 1U << 2U,
    LifecycleWithdrawState2 = 1U << 3U,
    LifecycleHostDormant = 1U << 4U,
};

enum InteractionFlag : std::uint32_t {
    InteractionActorIsSource = 1U << 0U,
    InteractionTargetIsParticipant = 1U << 1U,
    InteractionPairValid = 1U << 2U,
};

#pragma pack(push, 1)
struct State final {
    std::uint32_t version{5U};
    std::uint32_t nativeGeneration{}; // informational: generations differ per process
    std::uint64_t sessionCookie{};
    std::uint32_t worldGeneration{};
    std::uint32_t entityA{}, entityB{}, characterClass{}, birth{}, revision{};
    // Coarse authoritative Character convergence, not animation replication.
    // Native locomotion turns this intent into the local gait/animation.
    std::uint32_t stateFlags{};
    float position[3]{};
    float yaw{};
    float velocityX{}, velocityZ{};
    std::uint32_t moveType{};
    // Stable action identity/lifecycle metadata for the certified executable:
    // leaf definition vtable RVA plus a hash of the complete active BT path.
    // It is never interpreted as a serialized leaf or animation state.
    std::uint32_t actionTypeRva{}, actionPathHash{}, actionRevision{};
    std::uint32_t actionStatus{}, actionDepth{};
    // Ordered high-level Character-to-Character carrier selected by the Host.
    // The target is remapped locally before invoking the same retail boundary;
    // retail/CATHODE then produce animation, audio and gameplay consequences.
    // Payload/variant values are resolved outputs, never species-specific state.
    std::uint32_t interactionRevision{}, interactionTargetA{}, interactionTargetB{};
    std::uint32_t interactionFlags{}, interactionTransition{}, interactionTransitionFlags{};
    std::uint32_t interactionPayloadLow{}, interactionPayloadHigh{};
    std::int32_t interactionVariant{};
    float interactionTargetPosition[3]{};
    // Role execution is a second decision layer below ATOMIC_PERFORM_ROLE.
    // The Host exports only the admitted task identity; Clients use revisions
    // as authority barriers so stale local tasks can never fight PoseSync.
    std::uint32_t roleTaskRevision{}, roleTaskTypeRva{}, roleTaskKeyA{}, roleTaskKeyB{};
    // Lifecycle transitions cover vent/backstage/frontstage discontinuities.
    // They are not animation events: a revision means the proxy execution
    // context must be quiesced/rebound before normal convergence resumes.
    std::uint32_t lifecycleRevision{}, lifecycleFlags{};
};
struct Configuration final {
    std::uint32_t version{1U};
    Role role{Role::Retail};
    std::uint32_t nativeGeneration{}, worldGeneration{};
    std::uint64_t sessionCookie{};
};
#pragma pack(pop)
static_assert(sizeof(State) == 164U);
static_assert(sizeof(Configuration) == 24U);

bool Install(std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, GameThreadDispatcher& dispatcher,
    std::string& error);
bool Configure(std::span<const std::byte> payload) noexcept;
bool QueueRemote(std::span<const std::byte> payload) noexcept;
bool TryPopObserved(State& state) noexcept;
void PumpOnEngineThread() noexcept;
// SpawnCharacter creates the NPC-backed visual representation of a remote
// player. Reserve its runtime identity before native construction because the
// Character manager can update it re-entrantly before CharacterCreate returns.
void ObservePlayerRepresentationSpawn(std::uint32_t spawnKey) noexcept;
void ObservePlayerRepresentation(std::uintptr_t character) noexcept;
void ForgetCharacter(std::uintptr_t character) noexcept;
void ForgetSpawnKey(std::uint32_t spawnKey) noexcept;
void TransportLost() noexcept;
void BeginLogicalShutdown() noexcept;
// True while AISync, rather than retail AI, owns the NPC authority boundary.
bool OwnsAI() noexcept;
bool Installed() noexcept;
struct Counters final {
    std::uint64_t observed{}, applied{}, dropped{}, refused{}, suspended{}, faults{};
    std::uint64_t missingIdentity{};
    std::uint64_t managerUpdates{}, admitted{}, identityRejected{}, treeUnavailable{}, ownerMismatch{};
    std::uint64_t remoteQueued{};
    std::uint64_t poseCaptured{}, poseApplied{}, poseRejected{};
    std::uint64_t actionCaptured{}, actionTransitions{};
    std::uint64_t roleTaskCaptured{}, roleTaskTransitions{};
    std::uint64_t lifecycleCaptured{}, lifecycleApplied{}, lifecycleSnaps{}, executionBarriers{};
    std::uint64_t withdrawSamples{}, withdrawTransitions{}, withdrawApplied{};
    std::uint64_t interactionCaptured{}, interactionApplied{}, interactionRejected{}, interactionRemapped{};
    std::uint64_t pairedTransitionCaptured{}, pairedTransitionApplied{};
    std::uint64_t playerRepresentationsRegistered{}, provisionalPlayerActorsRetired{};
    std::uint32_t role{}, nativeGeneration{}, worldGeneration{};
    std::uint32_t lastEntityA{}, lastEntityB{}, lastCharacterClass{};
    std::uint32_t lastActionTypeRva{}, lastActionPathHash{}, lastActionStatus{};
    std::uint32_t lastRoleTaskTypeRva{}, lastRoleTaskKeyA{}, lastRoleTaskKeyB{};
    std::uint32_t lastLifecycleFlags{}, lastWithdrawState{};
    std::uint32_t lastInteractionTargetA{}, lastInteractionTargetB{};
    std::uint32_t lastInteractionTransition{};
    bool stable{};
};
Counters DiagnosticsSnapshot() noexcept;
// Call only from IPC/bootstrap workers, never from an engine detour.
void LogDiagnostics(Diagnostics& diagnostics);
} // namespace ai_sync
} // namespace aimp
