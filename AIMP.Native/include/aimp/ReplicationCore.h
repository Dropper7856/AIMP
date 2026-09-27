#pragma once

#include "aimp/Common.h"
#include "aimp/WorldReplicationTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {

class GameThreadDispatcher;
class LevelGenerationTracker;
class WorldEntityRegistry;

namespace replication_core {
// Nested retail boundaries. The outermost scheduler return seals causal segments.
void BeginCausalTurn() noexcept;
void EndCausalTurn() noexcept;

struct RuntimeDiagnostics final {
    bool initialized{};
    std::uint64_t version{};
    std::uint64_t initializationCount{};
    std::uint64_t shutdownCount{};
    std::uint64_t levelGenerationAtInitialize{};
    std::uint64_t environmentHookCount{};
    std::uint64_t pollingPassCount{};
    std::uint64_t queuedMutationCount{};
    std::uint64_t observedMutationCount{};
    std::uint64_t appliedMutationCount{};
    std::uint64_t outboundIntentCount{};
    std::uint64_t echoSuppressedCount{};
    std::uint64_t receivedMutationCount{};
    std::uint64_t acceptedMutationCount{};
    std::uint64_t duplicateRejectedCount{};
    std::uint64_t staleRejectedCount{};
    std::uint64_t generationRejectedCount{};
    std::uint64_t manifestRejectedCount{};
    std::uint64_t entityRejectedCount{};
    std::uint64_t queueRejectedCount{};
    std::uint64_t resolvedDispatchCount{};
    std::uint64_t runtimePendingCount{};
    std::uint64_t nativeFaultCount{};
    std::uint64_t resetCount{};
    std::uint64_t deferredRemoteApplyCount{};
    std::uint64_t deferredQueuedCount{};
    std::uint64_t deferredRetryCount{};
    std::uint64_t deferredExecutedCount{};
    std::uint64_t deferredExpiredCount{};
    std::uint64_t deferredOverflowCount{};
    std::uint64_t causalSegmentsSealed{}, causalSegmentsApplied{}, causalSegmentsFailed{}, causalSequenceGaps{};
    std::uint64_t causalLastCommittedSequence{}, causalDivergedAuthorityTransaction{};
    bool causalRecoveryRequired{};
    // Patch 4.3 / Patch 5: certified EmittedOutput or high-level CompositeInput
    // StateTransition multi-root transactions are reconciled and executed
    // strictly in ordinal order with a fresh state read before every member.
    // These counters distinguish that path from the historical
    // RejectStatefulMultiRoot fail-closed path.
    std::uint64_t orderedCarrierTransactionCount{}, orderedCarrierMemberCount{},
        orderedCarrierCommitCount{}, orderedCarrierFailureCount{};
    // Patch 4 WM09 active semantic reconciliation. Contracted State and
    // StateTransition roots are reconciled before Prepare; these fields expose
    // both the semantic candidate and the action actually enforced.
    std::uint64_t semanticProbeCount{}, semanticContractedProbeCount{};
    std::uint64_t semanticUnknownContractCount{}, semanticEventContractCount{},
        semanticStateContractCount{}, semanticStateTransitionContractCount{};
    std::uint64_t semanticReaderResolvedCount{}, semanticReaderRuntimeUnavailableCount{},
        semanticReaderUnsupportedCount{}, semanticReaderInvalidStateCount{},
        semanticReaderNativeFaultCount{};
    std::uint64_t semanticCandidateApplyCount{}, semanticCandidateSkipCount{},
        semanticCandidateMismatchCount{}, semanticCandidateAlreadyDesiredTransitionUnprovenCount{},
        semanticCandidateUnknownCount{};
    std::uint64_t semanticActionLegacyApplyCount{}, semanticActionApplyCount{},
        semanticActionSkipCount{}, semanticActionRetryCount{}, semanticActionRejectCount{},
        semanticActionTransitionUnprovenRejectCount{}, semanticActionStatefulMultiRootRejectCount{},
        semanticActionCarrierTransitionApplyCount{}, semanticTraceWriteCount{};
    bool semanticHasLast{};
    std::uint64_t semanticLastAuthoritySequence{}, semanticLastObservationId{},
        semanticLastAuthorityTransaction{};
    std::uint64_t semanticLastApplyBoundaryHigh{}, semanticLastApplyBoundaryLow{};
    std::uint32_t semanticLastApplyBoundaryOperation{};
    std::uint64_t semanticLastTargetHigh{}, semanticLastTargetLow{};
    std::uint32_t semanticLastTargetPrimitiveType{}, semanticLastOperation{};
    std::uint8_t semanticLastRootKind{}, semanticLastContract{}, semanticLastReadStatus{},
        semanticLastCandidateDecision{}, semanticLastAction{};
    bool semanticLastDesiredKnown{}, semanticLastDesired{},
        semanticLastExpectedBeforeKnown{}, semanticLastExpectedBefore{},
        semanticLastLocalBeforeKnown{}, semanticLastLocalBefore{},
        semanticLastCarrierBoundary{};
    bool controlledRemoteApplyProbeOnly{};
    bool controlledRemoteApplyFirstInteraction{};
    bool controlledRemoteApplyArmed{};
    std::uint64_t controlledRemoteApplyTargetHigh{};
    std::uint64_t controlledRemoteApplyTargetLow{};
    std::uint32_t controlledRemoteApplyTargetRootCount{};
    std::uint64_t controlledRemoteApplyArmCount{};
    std::uint64_t controlledRemoteApplyGameplayCandidateCount{};
    std::uint64_t controlledRemoteApplyGameplaySelectedCount{};
    std::uint64_t controlledRemoteApplyUnsafeContextSuppressedCount{};
    std::uint64_t controlledRemoteApplyNotArmedSuppressedCount{};
    std::uint64_t controlledRemoteApplyOtherTargetSuppressedCount{};
    std::uint64_t controlledRemoteApplyCrossTargetSelectedCount{};
    std::uint64_t controlledRemoteApplyBudgetSuppressedCount{};
    std::uint32_t controlledRemoteApplyLastSelectedInput{};
    std::uint64_t controlledRemoteApplyLastSelectedTargetHigh{};
    std::uint64_t controlledRemoteApplyLastSelectedTargetLow{};
    std::uint64_t controlledRemoteApplyLastSelectedSequence{};
    std::uint64_t controlledRemoteApplyParsedCount{};
    std::uint64_t controlledRemoteApplySelectedCount{};
    std::uint64_t controlledRemoteApplySuppressedCount{};
};

[[nodiscard]] constexpr ModuleReport Report() noexcept {
    return {
        "WorldSync2.ReplicationCore",
        ModuleStatus::Implemented,
        "v12.8 preserves the bounded host-authoritative v12.4 ordering model and resolves generic WorldEntityId mutations on the game thread. Active environment capture is type-agnostic: CATHODE input edges and typed message/blob records only; object-specific Door/Elevator adapters remain dormant compatibility code."};
}

bool Initialize(
    LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher,
    WorldEntityRegistry& registry,
    std::string& error) noexcept;

[[nodiscard]] bool Initialized() noexcept;
[[nodiscard]] RuntimeDiagnostics DiagnosticsSnapshot() noexcept;

[[nodiscard]] bool SubmitLocalIntent(
    const WorldEntityId& entity,
    WorldMutationKind requestedKind,
    std::uint32_t operation,
    std::span<const std::byte> payload = {}) noexcept;
[[nodiscard]] bool TryPopOutboundIntent(WorldIntent& output) noexcept;
void MarkOutboundIntentTransportFailure(const WorldIntent& intent) noexcept;

[[nodiscard]] bool ReceiveAuthoritativeMutation(
    const WorldMutation& mutation) noexcept;
[[nodiscard]] bool TryPopMutationResult(WorldMutationResult& output) noexcept;
[[nodiscard]] std::size_t PumpDeferredRemoteApplyOnEngineThread(
    std::size_t budget = 4U) noexcept;

[[nodiscard]] bool RemoteApplyActive() noexcept;
void ResetForGeneration(std::uint64_t generation) noexcept;
void BeginLogicalShutdown() noexcept;

} // namespace replication_core
} // namespace aimp
