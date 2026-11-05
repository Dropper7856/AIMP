#pragma once

#include "aimp/PlayerHidingPresentation.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

namespace aimp {
class SignatureResolver;
class WorldEntityRegistry;

namespace hiding_door_presentation {

// Sound is authored in the accompanying PLAYER clip's AUDIO metadata, not
// these environment door clips. Preserve the player clip's MDDB and use its
// native animation pipeline; do not synthesize a second SoundObject event here.

// COMMANDS sibling identities, in the retail uint32 little-endian form.
struct DoorSelection final {
    std::uint32_t icon{}, enter{}, exit{}, monitor{};
};
[[nodiscard]] constexpr DoorSelection Selection(const std::uint32_t kind) noexcept {
    if (kind == 1U) return {0x5DC33E3EU, 0x45A72956U, 0x3911F0F0U, 0xFFA58379U};
    if (kind == 2U) return {0x61B51D78U, 0x68FA9398U, 0x4492B8E9U, 0x63527A4DU};
    return {};
}
inline constexpr std::uint32_t kApplyStart = 0x40F9CC50U;
inline constexpr std::uint32_t kEntryEvent = 0xFDFA5FCBU;
inline constexpr std::uint32_t kExitEvent = 0x4F35B3A2U;
// Policy limit, not an animation duration. No certified environment seek API
// exists: stale snapshots must not restart their clip and sound from frame 0.
inline constexpr std::uint32_t kMaximumReplayAgeMilliseconds = 500U;

struct EdgeIdentity final { std::uint32_t kind{}, phase{}, icon{}, hierarchy{}; };
// Portable part of the scheduler predicate. Live class and manifest type
// checks remain mandatory in IsHidingDoorInput before accepting this identity.
[[nodiscard]] constexpr EdgeIdentity RecognizeInputEdge(
    const std::uint32_t targetGuid, const std::uint32_t targetHierarchy,
    const std::uint32_t sourceGuid, const std::uint32_t sourceHierarchy,
    const std::uint32_t parameter, const std::uint32_t input) noexcept {
    if (input != kApplyStart || targetHierarchy == 0U || targetHierarchy != sourceHierarchy) return {};
    for (const auto kind : {1U, 2U}) {
        const auto selection = Selection(kind);
        if (sourceGuid != selection.monitor) continue;
        if (targetGuid == selection.enter && parameter == kEntryEvent)
            return {kind, 1U, selection.icon, targetHierarchy};
        if (targetGuid == selection.exit && parameter == kExitEvent)
            return {kind, 3U, selection.icon, targetHierarchy};
    }
    return {};
}
[[nodiscard]] constexpr bool IsNewTransition(const std::uint32_t oldGeneration,
    const std::uint64_t oldTransition, const std::uint32_t generation,
    const std::uint64_t transition) noexcept {
    return transition != 0U && (generation != oldGeneration || transition > oldTransition);
}
[[nodiscard]] constexpr bool ReplayAgeAllowed(const std::uint32_t elapsed,
    const std::uint64_t queuedAge) noexcept {
    return elapsed <= kMaximumReplayAgeMilliseconds &&
        queuedAge <= kMaximumReplayAgeMilliseconds - elapsed;
}

[[nodiscard]] inline bool ValidSelection(const player_hiding_presentation::State& state) noexcept {
    return player_hiding_presentation::Valid(state) && state.phase != 0U &&
        state.hierarchyHash != 0U && state.entityShortGuid == Selection(state.kind).icon;
}

struct LocalSnapshot final {
    player_hiding_presentation::State state{};
    std::uint32_t levelGeneration{};
    // Native monotonic clock, for diagnostics only; never a network timestamp.
    std::uint64_t observedAtMilliseconds{};
};
struct DiagnosticsSnapshot final {
    std::uint64_t captured{}, queued{}, duplicate{}, baseline{}, stale{}, rejected{},
        unresolved{}, nativeFaults{}, generationDiscarded{};
    std::uint32_t lastTargetShortGuid{}, lastHierarchyHash{};
};

bool Initialize(const SignatureResolver& resolver, WorldEntityRegistry& registry,
    std::string& error) noexcept;
// IPC-safe. ownerToken is an opaque local lifecycle key, never dereferenced.
bool Publish(std::uintptr_t ownerToken, const player_hiding_presentation::State& state,
    std::string& error) noexcept;
void PumpAllOnEngineThread() noexcept;
void Forget(std::uintptr_t ownerToken) noexcept;
void ForgetAll() noexcept;
void ResetForGeneration() noexcept;
void BeginLogicalShutdown() noexcept;
[[nodiscard]] DiagnosticsSnapshot Diagnostics() noexcept;
// QueueInputDetour must execute retail directly while this is true, without
// submitting a second generic WS3 world root for this dedicated presentation.
[[nodiscard]] bool RemoteReplayActive() noexcept;

// Passive integration point AFTER retail QueueInput accepted an edge. Pass
// execution objects, not shared-ref wrappers. This neither triggers nor hooks
// anything. Only the exact same-instance monitor -> animation tuple is captured.
bool ObserveLocalInputEdge(const void* targetExecutionObject, const void* sourceExecutionObject,
    std::uint32_t schedulerParameterShortGuid, std::uint32_t targetInputShortGuid,
    double relativeDelaySeconds = 0.0) noexcept;
// Convenience form for the certified FUN_009271C0 observer argument layout.
bool ObserveAcceptedQueueInput(void** sourceSharedRef, const std::uint32_t* schedulerParameter,
    const void* targetDescriptor, double delaySeconds) noexcept;
// Non-mutating classifier for the FIRST QueueInputDetour gate: exact hiding
// door edges remain local retail + dedicated presentation, never a WS3 root.
[[nodiscard]] bool IsHidingDoorInput(void** sourceSharedRef,
    const std::uint32_t* schedulerParameter, const void* targetDescriptor,
    double delaySeconds) noexcept;
// Returns a value copy. elapsedMilliseconds is refreshed; alignment is zero
// until the parent combines this snapshot with its existing character reader.
[[nodiscard]] std::optional<LocalSnapshot> ReadLocalSnapshot() noexcept;

} // namespace hiding_door_presentation
} // namespace aimp
