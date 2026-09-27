#include "aimp/WorldSemanticReconciliation.h"

#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldInteractionAdapters.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace aimp::semantic_reconciliation {
namespace {

// STATIC CONFIRMED from the supplied retail AI.exe/AI.exe.c and retained
// historical diagnostics. The stable manifest target resolves to the outer
// CATHODE execution object. The LogicSwitch native subobject is +0x14 and its
// convergent boolean state byte is +0x0C inside that subobject.
constexpr std::uint32_t kLogicSwitchPrimitiveType = 0x0E423FCCU;
constexpr std::uint32_t kLogicSwitchSetTrueInputShortGuid = 0x5EB0E648U;
constexpr std::uint32_t kLogicSwitchSetFalseInputShortGuid = 0xD2B3CC2BU;
constexpr std::ptrdiff_t kLogicSwitchNativeOffset = 0x14;
constexpr std::ptrdiff_t kLogicSwitchStateOffset = 0x0C;

std::atomic<std::uint64_t> gProbeCount{0U};
std::atomic<std::uint64_t> gContractedProbeCount{0U};
std::atomic<std::uint64_t> gUnknownContractCount{0U};
std::atomic<std::uint64_t> gEventContractCount{0U};
std::atomic<std::uint64_t> gStateContractCount{0U};
std::atomic<std::uint64_t> gStateTransitionContractCount{0U};
std::atomic<std::uint64_t> gReaderResolvedCount{0U};
std::atomic<std::uint64_t> gReaderRuntimeUnavailableCount{0U};
std::atomic<std::uint64_t> gReaderUnsupportedCount{0U};
std::atomic<std::uint64_t> gReaderInvalidStateCount{0U};
std::atomic<std::uint64_t> gReaderNativeFaultCount{0U};
std::atomic<std::uint64_t> gCandidateApplyCount{0U};
std::atomic<std::uint64_t> gCandidateSkipCount{0U};
std::atomic<std::uint64_t> gCandidateMismatchCount{0U};
std::atomic<std::uint64_t> gCandidateAlreadyDesiredTransitionUnprovenCount{0U};
std::atomic<std::uint64_t> gCandidateUnknownCount{0U};
std::atomic<std::uint64_t> gActionLegacyApplyCount{0U};
std::atomic<std::uint64_t> gActionApplyCount{0U};
std::atomic<std::uint64_t> gActionSkipCount{0U};
std::atomic<std::uint64_t> gActionRetryCount{0U};
std::atomic<std::uint64_t> gActionRejectCount{0U};
std::atomic<std::uint64_t> gActionTransitionUnprovenRejectCount{0U};
std::atomic<std::uint64_t> gActionStatefulMultiRootRejectCount{0U};
std::atomic<std::uint64_t> gActionCarrierTransitionApplyCount{0U};
std::atomic<std::uint64_t> gTraceWriteCount{0U};

std::mutex gLastMutex;
ReconciliationResult gLast{};
bool gHasLast{};

std::mutex gTraceMutex;
HANDLE gTraceFile{INVALID_HANDLE_VALUE};
std::wstring gTracePath;

[[nodiscard]] bool ReadPointerSafely(const void* address, void*& output) noexcept {
    output = nullptr;
    if (address == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        output = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    output = *static_cast<void* const*>(address);
    return true;
#endif
}

[[nodiscard]] bool ReadU8Safely(const void* address, std::uint8_t& output) noexcept {
    output = 0U;
    if (address == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        output = *static_cast<const std::uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    output = *static_cast<const std::uint8_t*>(address);
    return true;
#endif
}

[[nodiscard]] std::optional<WorldCompositePublicInputCausalProfile> FindProfile(
    const WorldEntityRegistry& registry,
    const WorldEntityId& applyBoundaryEntity,
    const world_interactions::CompositePublicInputEvent& event) noexcept {
    switch (event.rootKind) {
    case world_interactions::PublicRootKind::CompositeInput:
        return registry.FindCompositePublicInputCausalProfile(
            applyBoundaryEntity, event.publicInputShortGuid);
    case world_interactions::PublicRootKind::EmittedOutput:
        return registry.FindEmittedOutputCausalProfile(
            applyBoundaryEntity, event.publicInputShortGuid);
    case world_interactions::PublicRootKind::ScheduledInput:
        return registry.FindScheduledInputCausalProfile(
            applyBoundaryEntity, event.publicInputShortGuid);
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool DeriveBooleanDesiredState(
    const std::uint32_t primitiveType,
    const std::uint32_t semanticOperation,
    bool& desired) noexcept {
    if (primitiveType != kLogicSwitchPrimitiveType) return false;
    if (semanticOperation == kLogicSwitchSetTrueInputShortGuid) {
        desired = true;
        return true;
    }
    if (semanticOperation == kLogicSwitchSetFalseInputShortGuid) {
        desired = false;
        return true;
    }
    return false;
}

[[nodiscard]] StateReadStatus ReadBooleanSemanticState(
    WorldEntityRegistry& registry,
    const WorldEntityId& target,
    const std::uint32_t primitiveType,
    const std::uint32_t semanticOperation,
    bool& value) noexcept {
    value = false;
    bool ignoredDesired = false;
    if (!DeriveBooleanDesiredState(primitiveType, semanticOperation, ignoredDesired))
        return StateReadStatus::ReaderUnsupported;

    auto resolved = registry.Resolve(target);
    if (!resolved.lease) {
        if (resolved.status == WorldRegistryResolveStatus::NativeFault)
            return StateReadStatus::NativeFault;
        if (resolved.status == WorldRegistryResolveStatus::RuntimeUnavailable ||
            resolved.status == WorldRegistryResolveStatus::PendingUntilAvailable)
            return StateReadStatus::RuntimeUnavailable;
        return StateReadStatus::InvalidState;
    }

    const auto live = registry.IdentifyRuntimeEntity(resolved.lease.Get());
    if (!live.has_value() || !(*live == target))
        return StateReadStatus::InvalidState;

    const auto* nativeSwitch = static_cast<const std::byte*>(resolved.lease.Get()) +
        kLogicSwitchNativeOffset;
    void* vtable = nullptr;
    if (!ReadPointerSafely(nativeSwitch, vtable) || vtable == nullptr)
        return StateReadStatus::NativeFault;

    std::uint8_t raw = 0U;
    if (!ReadU8Safely(nativeSwitch + kLogicSwitchStateOffset, raw))
        return StateReadStatus::NativeFault;
    if (raw > 1U) return StateReadStatus::InvalidState;
    value = raw != 0U;
    return StateReadStatus::Resolved;
}

[[nodiscard]] const char* ContractName(const WorldReplicationContractKind value) noexcept {
    switch (value) {
    case WorldReplicationContractKind::Event: return "Event";
    case WorldReplicationContractKind::State: return "State";
    case WorldReplicationContractKind::StateTransition: return "StateTransition";
    case WorldReplicationContractKind::Unknown:
    default: return "Unknown";
    }
}

[[nodiscard]] const char* ReadStatusName(const StateReadStatus value) noexcept {
    switch (value) {
    case StateReadStatus::NotApplicable: return "NotApplicable";
    case StateReadStatus::Resolved: return "Resolved";
    case StateReadStatus::RuntimeUnavailable: return "RuntimeUnavailable";
    case StateReadStatus::ReaderUnsupported: return "ReaderUnsupported";
    case StateReadStatus::InvalidState: return "InvalidState";
    case StateReadStatus::NativeFault: return "NativeFault";
    case StateReadStatus::NotAttempted:
    default: return "NotAttempted";
    }
}

[[nodiscard]] const char* DecisionName(const CandidateDecision value) noexcept {
    switch (value) {
    case CandidateDecision::ApplyRequired: return "ApplyRequired";
    case CandidateDecision::SkipAlreadySatisfied: return "SkipAlreadySatisfied";
    case CandidateDecision::Mismatch: return "Mismatch";
    case CandidateDecision::AlreadyDesiredTransitionUnproven:
        return "AlreadyDesiredTransitionUnproven";
    case CandidateDecision::Unknown:
    default: return "Unknown";
    }
}

[[nodiscard]] const char* ActionName(const ReconciliationAction value) noexcept {
    switch (value) {
    case ReconciliationAction::ApplyRequired: return "ApplyRequired";
    case ReconciliationAction::SkipAlreadySatisfied: return "SkipAlreadySatisfied";
    case ReconciliationAction::RetryRuntimeUnavailable: return "RetryRuntimeUnavailable";
    case ReconciliationAction::RejectMismatch: return "RejectMismatch";
    case ReconciliationAction::RejectTransitionUnproven: return "RejectTransitionUnproven";
    case ReconciliationAction::RejectReaderUnsupported: return "RejectReaderUnsupported";
    case ReconciliationAction::RejectInvalidState: return "RejectInvalidState";
    case ReconciliationAction::RejectNativeFault: return "RejectNativeFault";
    case ReconciliationAction::RejectStatefulMultiRoot: return "RejectStatefulMultiRoot";
    case ReconciliationAction::ApplyCarrierTransition: return "ApplyCarrierTransition";
    case ReconciliationAction::LegacyApply:
    default: return "LegacyApply";
    }
}

[[nodiscard]] std::string EntityText(const WorldEntityId& id) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0')
           << std::setw(16) << id.high << std::setw(16) << id.low;
    return stream.str();
}

[[nodiscard]] bool EnsureTraceOpenLocked() noexcept {
    if (gTraceFile != INVALID_HANDLE_VALUE) return true;

    wchar_t rootBuffer[32768]{};
    const auto length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", rootBuffer, _countof(rootBuffer));
    std::wstring root;
    if (length > 0U && length < _countof(rootBuffer)) {
        root.assign(rootBuffer, length);
    } else {
        wchar_t tempBuffer[32768]{};
        const auto tempLength = GetTempPathW(_countof(tempBuffer), tempBuffer);
        if (tempLength == 0U || tempLength >= _countof(tempBuffer)) return false;
        root.assign(tempBuffer, tempLength);
    }
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    root += L"AIMP";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\logs";
    CreateDirectoryW(root.c_str(), nullptr);
    gTracePath = root + L"\\AIMP.WS3.SemanticProbe-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
    gTraceFile = CreateFileW(
        gTracePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return gTraceFile != INVALID_HANDLE_VALUE;
}

void WriteTrace(const ReconciliationResult& result) noexcept {
    // Unknown contracts carry no semantic promise and are intentionally not
    // persisted. The counters still expose their volume.
    if (result.contractKind == WorldReplicationContractKind::Unknown) return;

    std::lock_guard lock(gTraceMutex);
    if (!EnsureTraceOpenLocked()) return;
    try {
        std::ostringstream stream;
        stream << "{\"version\":2"
               << ",\"passive\":false"
               << ",\"activeGate\":true"
               << ",\"pid\":" << GetCurrentProcessId()
               << ",\"tid\":" << GetCurrentThreadId()
               << ",\"writeTickMs\":" << GetTickCount64()
               << ",\"authoritySequence\":" << result.authoritySequence
               << ",\"observationId\":" << result.observationId
               << ",\"authorityTransaction\":" << result.authorityTransaction
               << ",\"rootKind\":" << static_cast<unsigned>(result.rootKind)
               << ",\"applyBoundaryEntity\":\"" << EntityText(result.applyBoundaryEntity) << '\"'
               << ",\"applyBoundaryOperation\":" << result.applyBoundaryOperation
               << ",\"contract\":\"" << ContractName(result.contractKind) << '\"'
               << ",\"contractCode\":" << static_cast<unsigned>(result.contractKind)
               << ",\"semanticTargetEntity\":\"" << EntityText(result.semanticTargetEntity) << '\"'
               << ",\"semanticTargetPrimitiveType\":" << result.semanticTargetPrimitiveType
               << ",\"semanticOperation\":" << result.semanticOperation
               << ",\"desiredKnown\":" << (result.desiredKnown ? "true" : "false")
               << ",\"desired\":" << (result.desired ? "true" : "false")
               << ",\"expectedBeforeKnown\":" << (result.expectedBeforeKnown ? "true" : "false")
               << ",\"expectedBefore\":" << (result.expectedBefore ? "true" : "false")
               << ",\"localBeforeKnown\":" << (result.localBeforeKnown ? "true" : "false")
               << ",\"localBefore\":" << (result.localBefore ? "true" : "false")
               << ",\"readStatus\":\"" << ReadStatusName(result.readStatus) << '\"'
               << ",\"readStatusCode\":" << static_cast<unsigned>(result.readStatus)
               << ",\"candidateDecision\":\"" << DecisionName(result.candidateDecision) << '\"'
               << ",\"candidateDecisionCode\":" << static_cast<unsigned>(result.candidateDecision)
               << ",\"action\":\"" << ActionName(result.action) << '\"'
               << ",\"actionCode\":" << static_cast<unsigned>(result.action)
               << ",\"carrierBoundary\":" << (result.carrierBoundary ? "true" : "false")
               << "}\r\n";
        const auto text = stream.str();
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(gTraceFile, zero, nullptr, FILE_BEGIN)) return;
        DWORD written = 0U;
        if (!WriteFile(gTraceFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written != static_cast<DWORD>(text.size())) return;
        static_cast<void>(SetEndOfFile(gTraceFile));
        gTraceWriteCount.fetch_add(1U, std::memory_order_relaxed);
    } catch (...) {
        // Diagnostics must never perturb the authoritative reconciliation path.
    }
}

void CountReadStatus(const StateReadStatus value) noexcept {
    switch (value) {
    case StateReadStatus::Resolved:
        gReaderResolvedCount.fetch_add(1U, std::memory_order_relaxed); break;
    case StateReadStatus::RuntimeUnavailable:
        gReaderRuntimeUnavailableCount.fetch_add(1U, std::memory_order_relaxed); break;
    case StateReadStatus::ReaderUnsupported:
        gReaderUnsupportedCount.fetch_add(1U, std::memory_order_relaxed); break;
    case StateReadStatus::InvalidState:
        gReaderInvalidStateCount.fetch_add(1U, std::memory_order_relaxed); break;
    case StateReadStatus::NativeFault:
        gReaderNativeFaultCount.fetch_add(1U, std::memory_order_relaxed); break;
    case StateReadStatus::NotAttempted:
    case StateReadStatus::NotApplicable:
    default:
        break;
    }
}

void CountDecision(const CandidateDecision value) noexcept {
    switch (value) {
    case CandidateDecision::ApplyRequired:
        gCandidateApplyCount.fetch_add(1U, std::memory_order_relaxed); break;
    case CandidateDecision::SkipAlreadySatisfied:
        gCandidateSkipCount.fetch_add(1U, std::memory_order_relaxed); break;
    case CandidateDecision::Mismatch:
        gCandidateMismatchCount.fetch_add(1U, std::memory_order_relaxed); break;
    case CandidateDecision::AlreadyDesiredTransitionUnproven:
        gCandidateAlreadyDesiredTransitionUnprovenCount.fetch_add(
            1U, std::memory_order_relaxed); break;
    case CandidateDecision::Unknown:
    default:
        gCandidateUnknownCount.fetch_add(1U, std::memory_order_relaxed); break;
    }
}

void CountAction(const ReconciliationAction value) noexcept {
    switch (value) {
    case ReconciliationAction::LegacyApply:
        gActionLegacyApplyCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::ApplyRequired:
        gActionApplyCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::SkipAlreadySatisfied:
        gActionSkipCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::RetryRuntimeUnavailable:
        gActionRetryCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::RejectTransitionUnproven:
        gActionTransitionUnprovenRejectCount.fetch_add(1U, std::memory_order_relaxed);
        gActionRejectCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::RejectStatefulMultiRoot:
        gActionStatefulMultiRootRejectCount.fetch_add(1U, std::memory_order_relaxed);
        gActionRejectCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::ApplyCarrierTransition:
        gActionCarrierTransitionApplyCount.fetch_add(1U, std::memory_order_relaxed);
        gActionApplyCount.fetch_add(1U, std::memory_order_relaxed); break;
    case ReconciliationAction::RejectMismatch:
    case ReconciliationAction::RejectReaderUnsupported:
    case ReconciliationAction::RejectInvalidState:
    case ReconciliationAction::RejectNativeFault:
        gActionRejectCount.fetch_add(1U, std::memory_order_relaxed); break;
    default:
        break;
    }
}

void RecordResult(const ReconciliationResult& result) noexcept {
    gProbeCount.fetch_add(1U, std::memory_order_relaxed);
    switch (result.contractKind) {
    case WorldReplicationContractKind::Event:
        gContractedProbeCount.fetch_add(1U, std::memory_order_relaxed);
        gEventContractCount.fetch_add(1U, std::memory_order_relaxed);
        break;
    case WorldReplicationContractKind::State:
        gContractedProbeCount.fetch_add(1U, std::memory_order_relaxed);
        gStateContractCount.fetch_add(1U, std::memory_order_relaxed);
        break;
    case WorldReplicationContractKind::StateTransition:
        gContractedProbeCount.fetch_add(1U, std::memory_order_relaxed);
        gStateTransitionContractCount.fetch_add(1U, std::memory_order_relaxed);
        break;
    case WorldReplicationContractKind::Unknown:
    default:
        gUnknownContractCount.fetch_add(1U, std::memory_order_relaxed);
        break;
    }
    CountReadStatus(result.readStatus);
    CountDecision(result.candidateDecision);
    CountAction(result.action);
    {
        std::lock_guard lock(gLastMutex);
        gLast = result;
        gHasLast = true;
    }
    WriteTrace(result);
}

} // namespace

bool IsCertifiedOrderedMultiRootCarrierMember(
    const WorldEntityRegistry& registry,
    const WorldEntityId& applyBoundaryEntity,
    const world_interactions::CompositePublicInputEvent& event) noexcept {
    if (event.rootCount <= 1U ||
        (event.rootKind != world_interactions::PublicRootKind::CompositeInput &&
         event.rootKind != world_interactions::PublicRootKind::EmittedOutput)) {
        return false;
    }
    const auto profile = FindProfile(registry, applyBoundaryEntity, event);
    return profile.has_value() && profile->CertifiedStateTransitionCarrierBoundary();
}

ReconciliationResult EvaluateBeforePrepare(
    WorldEntityRegistry& registry,
    const WorldEntityId& applyBoundaryEntity,
    const world_interactions::CompositePublicInputEvent& event,
    const std::uint64_t authoritySequence) noexcept {
    ReconciliationResult result{};
    result.authoritySequence = authoritySequence;
    result.observationId = event.observationId;
    result.authorityTransaction = event.authorityTransaction;
    result.applyBoundaryEntity = applyBoundaryEntity;
    result.applyBoundaryOperation = event.publicInputShortGuid;
    result.rootKind = static_cast<std::uint8_t>(event.rootKind);

    // DoorLockState is an explicit persistent state-set boundary. RequestLock
    // and RequestUnlock are idempotent desired states; authority ordering makes
    // the last accepted state authoritative without requiring a WM09 reader.
    if (event.rootKind == world_interactions::PublicRootKind::DoorLockState) {
        result.contractKind = WorldReplicationContractKind::State;
        result.semanticTargetEntity = event.targetEntity;
        result.semanticOperation = event.publicInputShortGuid;
        result.readStatus = StateReadStatus::NotAttempted;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::LegacyApply;
        RecordResult(result);
        return result;
    }

    // SchedulerEdge, TypedMessage and CanonicalCompositeInput are explicit
    // Event boundaries. They have no WM09 state-reader profile: replay is
    // delegated to the corresponding retail CATHODE primitive / manifest
    // childLink fan-out and exactly-once authority ordering.
    if (event.rootKind == world_interactions::PublicRootKind::SchedulerEdge ||
        event.rootKind == world_interactions::PublicRootKind::TypedMessage ||
        event.rootKind == world_interactions::PublicRootKind::CanonicalCompositeInput) {
        result.contractKind = WorldReplicationContractKind::Event;
        result.semanticTargetEntity = event.targetEntity;
        result.semanticOperation = event.publicInputShortGuid;
        result.readStatus = StateReadStatus::NotAttempted;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::LegacyApply;
        RecordResult(result);
        return result;
    }

    const auto profile = FindProfile(registry, applyBoundaryEntity, event);
    if (!profile.has_value()) {
        result.readStatus = StateReadStatus::NotAttempted;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::LegacyApply;
        RecordResult(result);
        return result;
    }

    result.contractKind = profile->replicationContractKind;
    result.carrierBoundary =
        profile->RelevanceDisposition() == WorldReplicationRelevanceDisposition::ReplicateCarrier;
    result.semanticTargetEntity = profile->semanticTargetEntity;
    result.semanticOperation = profile->semanticOperation;
    if (profile->semanticTargetEntity.Valid()) {
        result.semanticTargetPrimitiveType = registry.FindWorldEntityPrimitiveType(
            profile->semanticTargetEntity).value_or(0U);
    }

    if (result.contractKind == WorldReplicationContractKind::Unknown) {
        // Unknown remains the sole compatibility path. Patch 4 does not infer
        // semantics from names/GUIDs and therefore preserves historical replay.
        result.readStatus = StateReadStatus::NotAttempted;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::LegacyApply;
        RecordResult(result);
        return result;
    }

    if (result.contractKind == WorldReplicationContractKind::Event) {
        // Sequence/cause deduplication happens before this engine-thread gate.
        // Reaching here is a new authoritative occurrence and must execute once.
        result.readStatus = StateReadStatus::NotApplicable;
        result.candidateDecision = CandidateDecision::ApplyRequired;
        result.action = ReconciliationAction::ApplyRequired;
        RecordResult(result);
        return result;
    }

    // Patch 4.3 keeps the historical fail-closed rule for arbitrary stateful
    // multi-root transactions. Patch 5 extends the existing exception from
    // compiler-certified EmittedOutput carriers to compiler-certified
    // CompositeInput StateTransition carriers. ReplicationCore evaluates those
    // members one-by-one in transaction order, immediately before each
    // Prepare/Execute, so every LocalBefore read observes effects produced by
    // earlier members instead of using a stale transaction-wide snapshot.
    if (event.rootCount > 1U && !IsCertifiedOrderedMultiRootCarrierMember(
            registry, applyBoundaryEntity, event)) {
        result.readStatus = StateReadStatus::NotAttempted;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::RejectStatefulMultiRoot;
        RecordResult(result);
        return result;
    }

    bool desired = false;
    result.desiredKnown = DeriveBooleanDesiredState(
        result.semanticTargetPrimitiveType, result.semanticOperation, desired);
    result.desired = desired;
    if (!result.desiredKnown) {
        result.readStatus = StateReadStatus::ReaderUnsupported;
        result.candidateDecision = CandidateDecision::Unknown;
        result.action = ReconciliationAction::RejectReaderUnsupported;
        RecordResult(result);
        return result;
    }

    bool local = false;
    result.readStatus = ReadBooleanSemanticState(
        registry,
        result.semanticTargetEntity,
        result.semanticTargetPrimitiveType,
        result.semanticOperation,
        local);
    if (result.readStatus != StateReadStatus::Resolved) {
        result.candidateDecision = CandidateDecision::Unknown;
        switch (result.readStatus) {
        case StateReadStatus::RuntimeUnavailable:
            result.action = ReconciliationAction::RetryRuntimeUnavailable; break;
        case StateReadStatus::NativeFault:
            result.action = ReconciliationAction::RejectNativeFault; break;
        case StateReadStatus::InvalidState:
            result.action = ReconciliationAction::RejectInvalidState; break;
        case StateReadStatus::ReaderUnsupported:
            result.action = ReconciliationAction::RejectReaderUnsupported; break;
        default:
            result.action = ReconciliationAction::RejectMismatch; break;
        }
        RecordResult(result);
        return result;
    }
    result.localBeforeKnown = true;
    result.localBefore = local;

    if (result.contractKind == WorldReplicationContractKind::State) {
        if (local == desired) {
            result.candidateDecision = CandidateDecision::SkipAlreadySatisfied;
            result.action = ReconciliationAction::SkipAlreadySatisfied;
        } else {
            result.candidateDecision = CandidateDecision::ApplyRequired;
            result.action = ReconciliationAction::ApplyRequired;
        }
        RecordResult(result);
        return result;
    }

    if (result.contractKind == WorldReplicationContractKind::StateTransition) {
        result.expectedBeforeKnown = true;
        result.expectedBefore = !desired;
        if (local == result.expectedBefore) {
            result.candidateDecision = CandidateDecision::ApplyRequired;
            result.action = result.carrierBoundary
                ? ReconciliationAction::ApplyCarrierTransition
                : ReconciliationAction::ApplyRequired;
        } else if (local == desired) {
            // Local==Desired proves final convergence, not the transition-only
            // consequences. The low exact ScheduledInput therefore stays
            // rejected. A ReplicateCarrier is deliberately the higher retail
            // cause, so replaying it is the certified way to regenerate those
            // consequences without invoking the unsafe consumer boundary.
            result.candidateDecision = CandidateDecision::AlreadyDesiredTransitionUnproven;
            result.action = result.carrierBoundary
                ? ReconciliationAction::ApplyCarrierTransition
                : ReconciliationAction::RejectTransitionUnproven;
        } else {
            result.candidateDecision = CandidateDecision::Mismatch;
            result.action = ReconciliationAction::RejectMismatch;
        }
        RecordResult(result);
        return result;
    }

    result.candidateDecision = CandidateDecision::Unknown;
    result.action = ReconciliationAction::RejectMismatch;
    RecordResult(result);
    return result;
}

RuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    RuntimeDiagnostics result{};
    result.probeCount = gProbeCount.load(std::memory_order_acquire);
    result.contractedProbeCount = gContractedProbeCount.load(std::memory_order_acquire);
    result.unknownContractCount = gUnknownContractCount.load(std::memory_order_acquire);
    result.eventContractCount = gEventContractCount.load(std::memory_order_acquire);
    result.stateContractCount = gStateContractCount.load(std::memory_order_acquire);
    result.stateTransitionContractCount = gStateTransitionContractCount.load(std::memory_order_acquire);
    result.readerResolvedCount = gReaderResolvedCount.load(std::memory_order_acquire);
    result.readerRuntimeUnavailableCount = gReaderRuntimeUnavailableCount.load(std::memory_order_acquire);
    result.readerUnsupportedCount = gReaderUnsupportedCount.load(std::memory_order_acquire);
    result.readerInvalidStateCount = gReaderInvalidStateCount.load(std::memory_order_acquire);
    result.readerNativeFaultCount = gReaderNativeFaultCount.load(std::memory_order_acquire);
    result.candidateApplyCount = gCandidateApplyCount.load(std::memory_order_acquire);
    result.candidateSkipCount = gCandidateSkipCount.load(std::memory_order_acquire);
    result.candidateMismatchCount = gCandidateMismatchCount.load(std::memory_order_acquire);
    result.candidateAlreadyDesiredTransitionUnprovenCount =
        gCandidateAlreadyDesiredTransitionUnprovenCount.load(std::memory_order_acquire);
    result.candidateUnknownCount = gCandidateUnknownCount.load(std::memory_order_acquire);
    result.actionLegacyApplyCount = gActionLegacyApplyCount.load(std::memory_order_acquire);
    result.actionApplyCount = gActionApplyCount.load(std::memory_order_acquire);
    result.actionSkipCount = gActionSkipCount.load(std::memory_order_acquire);
    result.actionRetryCount = gActionRetryCount.load(std::memory_order_acquire);
    result.actionRejectCount = gActionRejectCount.load(std::memory_order_acquire);
    result.actionTransitionUnprovenRejectCount =
        gActionTransitionUnprovenRejectCount.load(std::memory_order_acquire);
    result.actionStatefulMultiRootRejectCount =
        gActionStatefulMultiRootRejectCount.load(std::memory_order_acquire);
    result.actionCarrierTransitionApplyCount =
        gActionCarrierTransitionApplyCount.load(std::memory_order_acquire);
    result.traceWriteCount = gTraceWriteCount.load(std::memory_order_acquire);
    {
        std::lock_guard lock(gLastMutex);
        result.hasLast = gHasLast;
        if (gHasLast) result.last = gLast;
    }
    return result;
}

void ResetForGeneration() noexcept {
    {
        std::lock_guard lock(gLastMutex);
        gLast = {};
        gHasLast = false;
    }
    // A level/world generation owns its semantic observations. Do not let a
    // later diagnostics export mistake a previous generation's last probe for
    // the current one when no contracted ROOT has executed yet.
    std::lock_guard traceLock(gTraceMutex);
    if (gTraceFile != INVALID_HANDLE_VALUE) {
        CloseHandle(gTraceFile);
        gTraceFile = INVALID_HANDLE_VALUE;
    }
    if (!gTracePath.empty()) static_cast<void>(DeleteFileW(gTracePath.c_str()));
    gTracePath.clear();
}

void Shutdown() noexcept {
    ResetForGeneration();
}

} // namespace aimp::semantic_reconciliation
