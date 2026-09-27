#pragma once

#include "aimp/WorldReplicationTypes.h"

#include <cstdint>

namespace aimp {

class WorldEntityRegistry;

namespace world_interactions {
struct CompositePublicInputEvent;
}

namespace semantic_reconciliation {

enum class StateReadStatus : std::uint8_t {
    NotAttempted = 0U,
    NotApplicable = 1U,
    Resolved = 2U,
    RuntimeUnavailable = 3U,
    ReaderUnsupported = 4U,
    InvalidState = 5U,
    NativeFault = 6U,
};

enum class CandidateDecision : std::uint8_t {
    Unknown = 0U,
    ApplyRequired = 1U,
    SkipAlreadySatisfied = 2U,
    Mismatch = 3U,
    AlreadyDesiredTransitionUnproven = 4U,
};

// Patch 4 keeps reconciliation authoritative before WorldPublicActions::Prepare and adds certified carrier application.
// LegacyApply preserves the historical path only for manifest entries that have
// no semantic contract. Contracted State/StateTransition entries are fail-closed
// whenever the reader cannot prove a safe Apply/Skip decision.
enum class ReconciliationAction : std::uint8_t {
    LegacyApply = 0U,
    ApplyRequired = 1U,
    SkipAlreadySatisfied = 2U,
    RetryRuntimeUnavailable = 3U,
    RejectMismatch = 4U,
    RejectTransitionUnproven = 5U,
    RejectReaderUnsupported = 6U,
    RejectInvalidState = 7U,
    RejectNativeFault = 8U,
    RejectStatefulMultiRoot = 9U,
    // Patch 4: a certified ReplicateCarrier is the higher CATHODE cause used
    // specifically to recover transition-only consequences without invoking
    // the lower ScheduledInput consumer directly.
    ApplyCarrierTransition = 10U,
};

struct ReconciliationResult final {
    std::uint64_t authoritySequence{};
    std::uint64_t observationId{};
    std::uint64_t authorityTransaction{};
    WorldEntityId applyBoundaryEntity{};
    std::uint32_t applyBoundaryOperation{};
    std::uint8_t rootKind{};
    WorldReplicationContractKind contractKind{WorldReplicationContractKind::Unknown};
    WorldEntityId semanticTargetEntity{};
    std::uint32_t semanticTargetPrimitiveType{};
    std::uint32_t semanticOperation{};
    StateReadStatus readStatus{StateReadStatus::NotAttempted};
    CandidateDecision candidateDecision{CandidateDecision::Unknown};
    ReconciliationAction action{ReconciliationAction::LegacyApply};
    bool desiredKnown{};
    bool desired{};
    bool expectedBeforeKnown{};
    bool expectedBefore{};
    bool localBeforeKnown{};
    bool localBefore{};
    bool carrierBoundary{};
};

struct RuntimeDiagnostics final {
    std::uint64_t probeCount{};
    std::uint64_t contractedProbeCount{};
    std::uint64_t unknownContractCount{};
    std::uint64_t eventContractCount{};
    std::uint64_t stateContractCount{};
    std::uint64_t stateTransitionContractCount{};
    std::uint64_t readerResolvedCount{};
    std::uint64_t readerRuntimeUnavailableCount{};
    std::uint64_t readerUnsupportedCount{};
    std::uint64_t readerInvalidStateCount{};
    std::uint64_t readerNativeFaultCount{};
    std::uint64_t candidateApplyCount{};
    std::uint64_t candidateSkipCount{};
    std::uint64_t candidateMismatchCount{};
    std::uint64_t candidateAlreadyDesiredTransitionUnprovenCount{};
    std::uint64_t candidateUnknownCount{};
    std::uint64_t actionLegacyApplyCount{};
    std::uint64_t actionApplyCount{};
    std::uint64_t actionSkipCount{};
    std::uint64_t actionRetryCount{};
    std::uint64_t actionRejectCount{};
    std::uint64_t actionTransitionUnprovenRejectCount{};
    std::uint64_t actionStatefulMultiRootRejectCount{};
    std::uint64_t actionCarrierTransitionApplyCount{};
    std::uint64_t traceWriteCount{};
    bool hasLast{};
    ReconciliationResult last{};
};

// Runs on the engine thread before dependency/context preflight and before
// WorldPublicActions::Prepare. The returned action is authoritative in Patch 4:
// Apply/LegacyApply may proceed to Prepare, Skip bypasses retail and is reported
// as an accepted transaction member, Retry requeues the whole transaction, and
// Reject fails the whole transaction without crossing retail.
[[nodiscard]] ReconciliationResult EvaluateBeforePrepare(
    WorldEntityRegistry& registry,
    const WorldEntityId& applyBoundaryEntity,
    const world_interactions::CompositePublicInputEvent& event,
    std::uint64_t authoritySequence) noexcept;

// Patch 4.3 structural certification for ordered stateful transactions. This
// performs manifest-only validation: no runtime dereference and no semantic
// state read. A multi-root transaction may use ordered reconciliation only
// when every remote member is a compiler-certified StateTransition carrier:
// either Patch 4's EmittedOutput boundary or Patch 5's higher CompositeInput.
[[nodiscard]] bool IsCertifiedOrderedMultiRootCarrierMember(
    const WorldEntityRegistry& registry,
    const WorldEntityId& applyBoundaryEntity,
    const world_interactions::CompositePublicInputEvent& event) noexcept;

[[nodiscard]] RuntimeDiagnostics DiagnosticsSnapshot() noexcept;
void ResetForGeneration() noexcept;
void Shutdown() noexcept;

} // namespace semantic_reconciliation
} // namespace aimp
