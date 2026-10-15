#pragma once

#include "aimp/AISync.h"
#include <array>
#include <cstdint>

namespace aimp::ai_sync {

struct ExecutionTransitionEntry final {
    std::uint32_t state, typeRva, entryRva;
    bool ignoredStackArgument{};
};

// Shared by capture and replay. No unvalidated semantic input is admitted.
inline constexpr std::array<ExecutionTransitionEntry, 7U> kExecutionTransitions{{
    {0x37U, 0x00EABE00U, 0x002577F0U},
    {0x03U, 0x00EABA40U, 0x00257980U},
    {0x09U, 0x00EAC1C0U, 0x00258190U},
    {0x10U, 0x00EADC18U, 0x00258080U},
    {0x5CU, 0x00EACDA8U, 0x00258390U},
    {0x51U, 0x00EAC608U, 0x00258420U},
    // 006625E0 -> 00661270: native locomotion stop. The constructor takes
    // its heading from the LOCAL movement object. Machine code ends in RET 4;
    // its forwarded argument is unused by 00661270, so replay supplies zero.
    {0x3BU, 0x00EABEC0U, 0x002625E0U, true},
}};

constexpr const ExecutionTransitionEntry* FindExecutionTransition(
    const std::uint32_t typeRva, const std::uint32_t state) noexcept {
    for (const auto& entry : kExecutionTransitions)
        if (entry.typeRva == typeRva && entry.state == state) return &entry;
    return nullptr;
}

enum class MovementOwner : std::uint8_t {
    None, Intent, Navigation, Execution, Paired, Neutral
};

constexpr bool AICharacterClass(const std::uint32_t type) noexcept {
    // Native template enum: 0 is PLAYER, 2/7 are ANDROID/ANDROID_HEAVY.
    // Fail closed for unadmitted families. The class describes appearance,
    // not network identity; PlayerSync participant ownership is checked
    // independently before NPC admission.
    return type == 1U || type == 2U || type == 3U || type == 4U ||
        type == 6U || type == 7U || type == 9U;
}

// The authority incarnation crosses the network. The runtime token only
// validates the admitted Character within this process; never compare it with
// another machine's token, even when they happen to have the same value.
constexpr bool ImpactTargetLifetimeMatches(const std::uint32_t receivedBirth,
    const std::uint32_t authoritativeBirth, const std::uint32_t currentRuntimeId,
    const std::uint32_t admittedRuntimeId) noexcept {
    return receivedBirth != 0U && receivedBirth == authoritativeBirth &&
        admittedRuntimeId != 0U && currentRuntimeId == admittedRuntimeId;
}

constexpr bool ExecutionCycleOwnsMovement(const std::uint32_t expectedType,
    const std::uint32_t expectedState, const std::uint32_t currentType,
    const std::uint32_t currentState, const std::uint8_t flags) noexcept {
    return expectedType != 0U && expectedType == currentType &&
        expectedState == currentState && (flags & 6U) == 0U;
}

constexpr std::uint32_t PairedPreparationEntry(const std::uint32_t stage) noexcept {
    // Native 4E execution transitions: both run the existing victim producer.
    // No stage/marker is fabricated and no local animation definition is chosen.
    return stage <= 1U ? 0x002638D0U : stage == 4U ? 0x00263900U : 0U;
}

constexpr bool UnclaimedPairLost(const bool busy, const bool sourcePresent,
    const bool victimPresent, const bool claimed) noexcept {
    return !busy && !sourcePresent && !victimPresent && !claimed;
}

constexpr bool HostSuspendsNavigation(const State& state) noexcept {
    // A live goal may remain queued while retail explicitly stops its physical
    // cycle. Finishing that local stop is not permission to resume the goal.
    return (state.stateFlags & ExecutionValid) != 0U && (state.stateFlags & Moving) == 0U &&
        state.executionCycleTypeRva == 0x00EABEC0U && state.executionCycleState == 0x3BU;
}

constexpr bool PairAdmissionPending(const State& state,
    const std::uint32_t completedPair, const std::uint32_t completedTransition) noexcept {
    return (state.stateFlags & InteractionValid) != 0U &&
        state.interactionPairRevision != 0U && state.interactionTransition != 0U &&
        (state.interactionPairRevision != completedPair ||
            state.interactionTransitionRevision != completedTransition) &&
        ((state.interactionFlags & (InteractionCycleActive | InteractionSourceCycleRetained)) != 0U ||
            state.outcome.revision != 0U);
}

constexpr bool NavigationAuthorityAdvanced(const std::uint32_t flags,
    const std::uint32_t revision, const std::uint32_t requestRevision,
    const std::uint32_t releasedRevision, const std::uint32_t releasedRequestRevision) noexcept {
    return flags == 1U && (revision > releasedRevision || requestRevision > releasedRequestRevision);
}

constexpr bool HostLeftPairedExecution(const bool executionValid, const bool executionPaired,
    const bool interactionActive, const bool sourceRetained) noexcept {
    return executionValid && !executionPaired && !interactionActive && !sourceRetained;
}

// Metadata gives no local ownership until replay succeeds for this revision.
constexpr MovementOwner ChooseMovementOwner(const bool paired, const bool neutral,
    const bool executionApplied, const bool navigationAvailable) noexcept {
    if (paired) return MovementOwner::Paired;
    if (neutral) return MovementOwner::Neutral;
    if (executionApplied) return MovementOwner::Execution;
    if (navigationAvailable) return MovementOwner::Navigation;
    return MovementOwner::Intent;
}

} // namespace aimp::ai_sync
