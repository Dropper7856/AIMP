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
