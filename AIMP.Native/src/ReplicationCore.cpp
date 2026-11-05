#include "aimp/ReplicationCore.h"
#include "aimp/CausalPublicationPolicy.h"
#include "aimp/OrderedTransactionBatch.h"

#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldEntityRegistry.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/WorldSemanticReconciliation.h"
#include "aimp/WorldAnimatedAdapters.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aimp::replication_core {
namespace {

constexpr std::size_t kIntentCapacity = kWorldIntentQueueCapacity;
constexpr std::size_t kResultCapacity = 512U;
constexpr std::uint64_t kDispatchLifetimeMs = 1000U;
constexpr std::size_t kDeferredRemoteApplyCapacity = 256U;
constexpr std::size_t kDeferredRemoteApplyPumpBudget = 4U;
constexpr std::uint64_t kDeferredRemoteApplyLifetimeMs = 1500U;
constexpr std::uint64_t kDeferredRemoteApplyRetryMs = 50U;
constexpr std::uint64_t kWorldMutationRequestBase = 0x5700000000000000ULL;
struct PublicCauseKey final {
    std::uint64_t peer{}, observation{};
    bool operator==(const PublicCauseKey&) const noexcept = default;
};
struct PublicCauseHash final {
    std::size_t operator()(const PublicCauseKey& key) const noexcept {
        const auto value = key.peer ^ key.observation ^ (key.observation >> 32U);
        return static_cast<std::size_t>(value);
    }
};
// Bounded duplicate memory only. TCP ordering plus authoritative sequence
// validation are the primary replay guards; old cause IDs can therefore be
// evicted instead of permanently blocking WorldSync after a few minutes.
std::unordered_set<PublicCauseKey, PublicCauseHash> gPublicCauses;
std::deque<PublicCauseKey> gPublicCauseOrder;
constexpr std::size_t kMaximumRecentPublicCauses = 8192U;

void RememberPublicCauseLocked(const PublicCauseKey& key) {
    if (!gPublicCauses.insert(key).second) return;
    gPublicCauseOrder.push_back(key);
    while (gPublicCauseOrder.size() > kMaximumRecentPublicCauses) {
        gPublicCauses.erase(gPublicCauseOrder.front());
        gPublicCauseOrder.pop_front();
    }
}

struct StateKey final {
    WorldEntityId entity{};
    std::uint32_t operation{};
    WorldMutationKind kind{WorldMutationKind::State};

    [[nodiscard]] bool operator==(const StateKey& other) const noexcept = default;
};

struct StateKeyHash final {
    [[nodiscard]] std::size_t operator()(const StateKey& value) const noexcept {
        auto hash = static_cast<std::size_t>(value.entity.high ^ (value.entity.high >> 32U));
        hash ^= static_cast<std::size_t>(value.entity.low ^ (value.entity.low >> 32U)) +
            0x9E3779B9U + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::size_t>(value.operation) +
            0x9E3779B9U + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::size_t>(value.kind) +
            0x9E3779B9U + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

template <typename T, std::size_t Capacity>
class BoundedRing final {
public:
    [[nodiscard]] bool Push(const T& value) noexcept {
        std::lock_guard lock(mutex_);
        if (count_ + reserved_ >= Capacity) { ++rejected_; return false; }
        storage_[(head_ + count_) % Capacity] = value;
        ++count_;
        highWater_ = (std::max)(highWater_, count_ + reserved_);
        return true;
    }

    // Capacity is reserved before a local ROOT claims its descendants. The
    // worker cannot observe a fragment until its whole causal turn is sealed.
    [[nodiscard]] std::uint64_t Reserve() noexcept {
        std::lock_guard lock(mutex_);
        if (count_ + reserved_ >= Capacity) { ++rejected_; return 0U; }
        ++reserved_;
        highWater_ = (std::max)(highWater_, count_ + reserved_);
        return epoch_;
    }
    void Cancel(std::uint64_t ticket) noexcept {
        std::lock_guard lock(mutex_);
        if (ticket == epoch_ && reserved_ != 0U) --reserved_;
    }
    [[nodiscard]] bool PushReserved(const T& value, std::uint64_t ticket) noexcept {
        std::lock_guard lock(mutex_);
        if (ticket != epoch_ || reserved_ == 0U) return false;
        --reserved_;
        storage_[(head_ + count_) % Capacity] = value;
        ++count_;
        return true;
    }

    template <typename Iterator>
    [[nodiscard]] bool PublishReserved(Iterator first, Iterator last) noexcept {
        std::lock_guard lock(mutex_);
        std::size_t total = 0U;
        for (auto it = first; it != last; ++it) {
            if (it->ticket != epoch_) return false;
            ++total;
        }
        if (total > reserved_ || total + count_ > Capacity) return false;
        for (auto it = first; it != last; ++it) {
            storage_[(head_ + count_) % Capacity] = it->intent;
            ++count_;
        }
        reserved_ -= total;
        return true;
    }

    [[nodiscard]] bool Pop(T& output) noexcept {
        std::lock_guard lock(mutex_);
        if (count_ == 0U) return false;
        output = storage_[head_];
        head_ = (head_ + 1U) % Capacity;
        --count_;
        return true;
    }

    void Clear() noexcept {
        std::lock_guard lock(mutex_);
        head_ = 0U;
        count_ = reserved_ = 0U;
        highWater_ = rejected_ = 0U;
        if (++epoch_ == 0U) ++epoch_;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        std::lock_guard lock(mutex_);
        return count_;
    }
    [[nodiscard]] std::size_t Reserved() const noexcept {
        std::lock_guard lock(mutex_);
        return reserved_;
    }
    [[nodiscard]] std::size_t HighWater() const noexcept {
        std::lock_guard lock(mutex_);
        return highWater_;
    }
    [[nodiscard]] std::size_t Rejected() const noexcept {
        std::lock_guard lock(mutex_);
        return rejected_;
    }

private:
    mutable std::mutex mutex_;
    std::array<T, Capacity> storage_{};
    std::size_t head_{};
    std::size_t count_{};
    std::size_t reserved_{};
    std::size_t highWater_{}, rejected_{};
    std::uint64_t epoch_{1U};
};

std::atomic<bool> gInitialized{false};
std::atomic<std::uint64_t> gVersion{0};
std::atomic<std::uint64_t> gInitializationCount{0};
std::atomic<std::uint64_t> gShutdownCount{0};
std::atomic<std::uint64_t> gLevelGenerationAtInitialize{0};
std::atomic<std::uint64_t> gLocalIntentSequence{1};
// Synchronous origin belongs to this engine thread; deferred work carries its own lineage.
thread_local std::uint32_t gRemoteApplyDepth = 0U;

std::atomic<std::uint64_t> gOutboundIntentCount{0};
std::atomic<std::uint64_t> gOutboundPoppedCount{0};
std::atomic<std::uint64_t> gAppliedMutationCount{0};
std::atomic<std::uint64_t> gEchoSuppressedCount{0};
std::atomic<std::uint64_t> gReceivedMutationCount{0};
std::atomic<std::uint64_t> gAcceptedMutationCount{0};
std::atomic<std::uint64_t> gDuplicateRejectedCount{0};
std::atomic<std::uint64_t> gStaleRejectedCount{0};
std::atomic<std::uint64_t> gGenerationRejectedCount{0};
std::atomic<std::uint64_t> gManifestRejectedCount{0};
std::atomic<std::uint64_t> gEntityRejectedCount{0};
std::atomic<std::uint64_t> gQueueRejectedCount{0};
std::atomic<std::uint64_t> gResolvedDispatchCount{0};
std::atomic<std::uint64_t> gRuntimePendingCount{0};
std::atomic<std::uint64_t> gNativeFaultCount{0};
std::atomic<std::uint64_t> gResetCount{0};
std::atomic<std::uint64_t> gDeferredQueuedCount{0};
std::atomic<std::uint64_t> gDeferredRetryCount{0};
std::atomic<std::uint64_t> gDeferredExecutedCount{0};
std::atomic<std::uint64_t> gDeferredExpiredCount{0};
std::atomic<std::uint64_t> gDeferredOverflowCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyParsedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplySelectedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplySuppressedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyArmCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyGameplayCandidateCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyGameplaySelectedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyUnsafeContextSuppressedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyNotArmedSuppressedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyOtherTargetSuppressedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyCrossTargetSelectedCount{0};
std::atomic<std::uint64_t> gControlledRemoteApplyBudgetSuppressedCount{0};
std::mutex gControlledRemoteApplyMutex;
bool gControlledRemoteApplyArmed{};
WorldEntityId gControlledRemoteApplyTarget{};
std::uint64_t gControlledRemoteApplyArmSequence{};
std::uint64_t gControlledRemoteApplyBurstUntil{};
std::uint32_t gControlledRemoteApplyRootCount{};
std::uint32_t gControlledRemoteApplyLastSelectedInput{};
WorldEntityId gControlledRemoteApplyLastSelectedTarget{};
std::uint64_t gControlledRemoteApplyLastSelectedSequence{};

struct DeferredRemoteApply final {
    WorldMutation mutation{};
    world_interactions::CompositePublicInputEvent root{};
    std::uint64_t receivedAt{};
    std::uint64_t expiresAt{};
    std::uint64_t nextRetryAt{};
    bool pendingRecorded{};
    bool suppressLocalEcho{};
};

std::mutex gDeferredRemoteApplyMutex;
std::deque<DeferredRemoteApply> gDeferredRemoteApplies;
std::size_t gReservedRemoteRoots{}, gInFlightRemoteRoots{};
world_interactions::CompositePublicInputEvent gIngressRoot{};
std::uint64_t gIngressPeer{};
std::uint16_t gIngressNextOrdinal{};

LevelGenerationTracker* gGenerations{};
GameThreadDispatcher* gDispatcher{};
WorldEntityRegistry* gRegistry{};
BoundedRing<WorldIntent, kIntentCapacity> gOutboundIntents;
BoundedRing<WorldMutationResult, kResultCapacity> gResults;
struct StagedRoot final { WorldIntent intent{}; std::uint64_t ticket{}; std::uint64_t causalSerial{}; };
thread_local std::uint32_t gCausalTurnDepth{};
thread_local std::vector<StagedRoot> gStagedRoots;
std::atomic<std::uint64_t> gSegmentSerial{1U};
std::mutex gOutboundSubmitMutex;
std::atomic<std::uint64_t> gSequenceGaps{}, gSealedSegments{}, gAppliedSegments{}, gFailedSegments{}, gSkippedSegments{};
std::atomic<std::uint64_t> gOrderedCarrierTransactionCount{}, gOrderedCarrierMemberCount{},
    gOrderedCarrierCommitCount{}, gOrderedCarrierFailureCount{};
thread_local bool gPumpingCausalSegment{};
std::mutex gOrderingMutex;
std::uint64_t gLastCommittedSequence{};
bool gCausalRecoveryRequired{};
std::uint64_t gDivergedAuthorityTransaction{};
std::unordered_map<StateKey, std::uint64_t, StateKeyHash> gStateRevisions;

void TouchVersion() noexcept {
    gVersion.fetch_add(1U, std::memory_order_acq_rel);
}

void MarkCausalRecoveryRequired(const std::uint64_t authorityTransaction = 0U) noexcept {
    std::lock_guard lock(gOrderingMutex);
    gCausalRecoveryRequired = true;
    gDivergedAuthorityTransaction = authorityTransaction;
}

[[nodiscard]] bool ValidKind(const WorldMutationKind kind) noexcept {
    return kind == WorldMutationKind::State ||
        kind == WorldMutationKind::Event ||
        kind == WorldMutationKind::Timeline;
}

[[nodiscard]] bool ValidFlags(const std::uint8_t flags) noexcept {
    constexpr auto kKnown = static_cast<std::uint8_t>(
        WorldMutationFlagReliable | WorldMutationFlagReplaceable |
        WorldMutationFlagReplayable | WorldMutationFlagSyntheticProbe);
    return (flags & static_cast<std::uint8_t>(~kKnown)) == 0U;
}

[[nodiscard]] bool ValidMutation(const WorldMutation& value) noexcept {
    if (!value.manifest.Valid() || !value.entity.Valid() ||
        value.levelGeneration == 0U || value.sequence == 0U ||
        value.timelineTick == 0U || value.operation == 0U ||
        value.sourcePeer == 0U ||
        !ValidKind(value.kind) || !ValidFlags(value.flags) ||
        !value.payload.Valid() ||
        (value.flags & WorldMutationFlagReliable) == 0U) return false;
    const auto replayFlags = static_cast<std::uint8_t>(
        WorldMutationFlagReplaceable | WorldMutationFlagReplayable);
    if (value.kind == WorldMutationKind::Event) {
        return value.revision == 0U && (value.flags & replayFlags) == 0U;
    }
    return value.revision != 0U && (value.flags & replayFlags) == replayFlags;
}

void PushResult(const WorldMutationResult& result) noexcept {
    if (!gResults.Push(result)) {
        gQueueRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
    }
    TouchVersion();
}

[[nodiscard]] WorldMutationResult Rejected(
    const WorldMutation& mutation,
    const WorldMutationResultCode code) noexcept {
    return {mutation.sequence, code, false, false, mutation.nativeDispatchId};
}

void CountRejection(const WorldMutationResultCode code) noexcept {
    switch (code) {
    case WorldMutationResultCode::DuplicateRejected:
        gDuplicateRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::StaleRejected:
        gStaleRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::GenerationRejected:
        gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::ManifestRejected:
        gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::EntityRejected:
        gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::QueueRejected:
        gQueueRejectedCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::RuntimePending:
        gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::NativeFault:
        gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel); break;
    case WorldMutationResultCode::Accepted:
    default:
        break;
    }
}

[[nodiscard]] std::uint64_t TickMs() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

class RemoteApplyScope final {
public:
    RemoteApplyScope() noexcept {
        ++gRemoteApplyDepth;
    }
    ~RemoteApplyScope() noexcept {
        --gRemoteApplyDepth;
    }
};

[[nodiscard]] std::uint8_t PublicRootPendingReason(
    const WorldMutation& mutation,
    const world_interactions::CompositePublicInputEvent& root) noexcept {
    if (gRegistry == nullptr) return 1U;
    const auto pendingEntity = [](const WorldEntityId& id) {
        const auto resolved = gRegistry->Resolve(id);
        return resolved.status == WorldRegistryResolveStatus::PendingUntilAvailable ||
            resolved.status == WorldRegistryResolveStatus::RuntimeUnavailable;
    };
    if (pendingEntity(mutation.entity)) return 1U;
    if (pendingEntity(root.targetEntity)) return 2U;
    if (root.contextKind == world_interactions::PublicContextKind::NativeContextChain) {
        for (std::size_t i = 0; i < root.contextFrameCount; ++i) {
            bool nativeFault = false;
            auto lease = gRegistry->ResolveRuntimeReference(root.contextFrames[i].runtimeKey, nativeFault);
            if (!nativeFault && !lease) return 3U;
        }
    }
    if (root.actorKind == world_interactions::PublicActorKind::WorldEntity &&
        pendingEntity(root.actorEntity)) return 4U;
    return 0U;
}


WorldMutationResult ExecuteMutationOnEngineThread(
    const WorldMutation& mutation,
    const std::uint64_t receivedAt,
    const std::shared_ptr<world_actions::PreparedPublicRoot>& preparedRoot = {}) noexcept {
    RemoteApplyScope scope;
    world_actions::SetRemoteCauseEnvelope(mutation.sourcePeer, mutation.sequence);
    WorldMutationResult result{
        mutation.sequence, WorldMutationResultCode::Accepted, false, false, mutation.nativeDispatchId};
    const auto payload = std::span<const std::byte>(
        mutation.payload.bytes.data(), mutation.payload.size);

    world_animated::DoorActionPayload doorActionPayload{};
    const bool doorAction =
        mutation.operation == world_animated::kOperationDoorAction;
    if (doorAction) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_animated::DecodeDoorActionPayload(payload, doorActionPayload)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        if (world_animated::ConsumeLocalDoorObservation(doorActionPayload.observationId)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    world_animated::AnimatedInputEdge animatedEdge{};
    const bool animatedInputEdge =
        mutation.operation == world_animated::kOperationAnimatedInputEdge;
    if (animatedInputEdge) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_animated::DecodeAnimatedInputEdgePayload(payload, animatedEdge)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        // v12.7 captures an edge only after retail CATHODE has already
        // queued it locally. Suppress the authoritative reflection (or a
        // matching optimistic local edge on another peer) before resolving.
        if (world_animated::ConsumeLocalAnimatedMatch(animatedEdge, mutation.entity)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    // WS3 strict accepts only the certified public ROOT operation from the
    // generic CATHODE family. Legacy WS2 consequence-level operations are
    // rejected even if stale traffic somehow survives transport/authority.
    const bool legacyGenericCathodeOperation =
        mutation.operation == world_interactions::kOperationObservedInputEdge ||
        mutation.operation == world_interactions::kOperationObservedCallbackInputEdge ||
        mutation.operation == world_interactions::kOperationObservedCharacterCallbackInputEdge ||
        mutation.operation == world_interactions::kOperationObservedTypedMessage ||
        mutation.operation == world_interactions::kOperationObservedLogicSwitchState;
    if (world_actions::Installed() && legacyGenericCathodeOperation) {
        result.code = WorldMutationResultCode::EntityRejected;
        gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
        PushResult(result);
        return result;
    }

    world_interactions::ObservedInputEdge observedEdge{};
    const bool observedInteractionEdge =
        mutation.operation == world_interactions::kOperationObservedInputEdge;
    if (observedInteractionEdge) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeObservedInputEdgePayload(payload, observedEdge)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        // The local retail scheduler already executed this exact edge before
        // it became a host-authoritative intent. Acknowledge the reflection
        // on the origin process without queueing the CATHODE input twice.
        if (world_interactions::ConsumeLocalObservation(observedEdge.observationId)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    world_interactions::CompositePublicInputEvent compositePublicInput{};
    const bool compositePublicInputMutation =
        mutation.operation == world_interactions::kOperationCompositePublicInput;
    if (compositePublicInputMutation) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeCompositePublicInputPayload(
                payload, compositePublicInput) ||
            (world_actions::Installed() && !compositePublicInput.retailRoot) ||
            !(compositePublicInput.retailRoot
                ? world_actions::PublicRootManifestAdmissionAllowed(*gRegistry, mutation.entity, compositePublicInput)
                : gRegistry->IsCompositePublicInputAllowed(
                    mutation.entity, compositePublicInput.publicInputShortGuid))) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        // Non-AwaitAuthority ROOTs already executed on their origin, so their
        // authoritative reflection consumes only the observation ID there. An
        // AwaitAuthority ROOT deliberately has no local execution/observation
        // and therefore continues to normal injection on the initiating Client.
        if ((compositePublicInput.flags & world_actions::kPublicRootFlagAwaitAuthority) == 0U &&
            world_actions::IsLocalPeer(mutation.sourcePeer) &&
            world_interactions::ConsumeLocalObservation(compositePublicInput.observationId)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    world_interactions::ObservedCharacterCallbackInputEdge characterCallbackEdge{};
    const bool observedCharacterCallbackEdge =
        mutation.operation == world_interactions::kOperationObservedCharacterCallbackInputEdge;
    if (observedCharacterCallbackEdge) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeObservedCharacterCallbackInputEdgePayload(
                payload, characterCallbackEdge)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        if (world_interactions::ConsumeLocalObservation(characterCallbackEdge.observationId) ||
            world_interactions::ConsumeLocalCharacterCallbackMatch(
                characterCallbackEdge, mutation.entity)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    world_interactions::ObservedCallbackInputEdge callbackEdge{};
    const bool observedCallbackInteractionEdge =
        mutation.operation == world_interactions::kOperationObservedCallbackInputEdge;
    if (observedCallbackInteractionEdge) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeObservedCallbackInputEdgePayload(payload, callbackEdge)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        // The origin process already executed the retail callback edge before
        // submitting the intent. Consume its authoritative reflection without
        // queueing the CATHODE callback twice on that same process.
        if (world_interactions::ConsumeLocalObservation(callbackEdge.observationId)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }

        // Fix3.3 fail-safe: callback-bearing mutations from Fix3/Fix3.2 are
        // intentionally accepted as a no-op and never reach registry/native
        // callback dispatch. This protects clients from stale/in-flight or
        // mixed-version traffic while the callback ABI is being proven.
        result.entityResolved = false;
        result.dispatched = false;
        TouchVersion();
        PushResult(result);
        return result;
    }

    world_interactions::ObservedLogicSwitchState logicSwitchState{};
    const bool observedLogicSwitchState =
        mutation.operation == world_interactions::kOperationObservedLogicSwitchState;
    if (observedLogicSwitchState) {
        if (mutation.kind != WorldMutationKind::State ||
            !world_interactions::DecodeObservedLogicSwitchStatePayload(
                payload, logicSwitchState)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        if (world_interactions::ConsumeLocalLogicSwitchState(
                logicSwitchState.observationId)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }
    }

    world_interactions::ObservedTypedMessage typedMessage{};
    std::span<const std::byte> typedMessageData{};
    const bool observedTypedMessage =
        mutation.operation == world_interactions::kOperationObservedTypedMessage;
    if (observedTypedMessage) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeObservedTypedMessagePayload(
                payload, typedMessage, typedMessageData)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
            return result;
        }
        if (world_interactions::ConsumeLocalTypedMessage(
                typedMessage, typedMessageData)) {
            result.entityResolved = true;
            result.dispatched = true;
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            TouchVersion();
            PushResult(result);
            return result;
        }

        // Fix3.14 diagnostic-only preflight. This runs after local-echo
        // suppression but before native target resolution, so the Client
        // can prove whether stable IDs map to local RuntimeKeys even if the
        // runtime object is temporarily pending. No replay decision is
        // changed here.
        const auto sourceWorld =
            gRegistry->FindRuntimeWorldEntityId(typedMessage.sourceRuntimeKey);
        const auto targetWorld =
            gRegistry->FindRuntimeWorldEntityId(typedMessage.targetRuntimeKey);
        const auto localTargetKey =
            gRegistry->FindRuntimeWorldEntityKey(mutation.entity);
        const bool rawGatePreflight = sourceWorld.has_value() &&
            targetWorld.has_value() && localTargetKey.has_value() &&
            mutation.entity == *targetWorld &&
            *localTargetKey == typedMessage.targetRuntimeKey;
        world_interactions::ObserveRemoteTypedIdentity(
            typedMessage, typedMessageData.size(), mutation.entity,
            localTargetKey.value_or(CathodeRuntimeKey{}), rawGatePreflight);
    }

    if (preparedRoot && compositePublicInputMutation && compositePublicInput.retailRoot) {
        result.entityResolved = true;
        const auto executed = world_actions::ExecutePrepared(*preparedRoot);
        result.dispatched = executed.queued;
        switch (executed.status) {
        case world_interactions::ExecuteStatus::Queued:
            if (result.dispatched) gAppliedMutationCount.fetch_add(1U, std::memory_order_relaxed);
            break;
        case world_interactions::ExecuteStatus::RuntimeUnavailable:
            result.code = WorldMutationResultCode::QueueRejected; break;
        case world_interactions::ExecuteStatus::NativeFault:
            result.code = WorldMutationResultCode::NativeFault; break;
        default:
            result.code = WorldMutationResultCode::EntityRejected; break;
        }
        if (result.code != WorldMutationResultCode::Accepted) CountRejection(result.code);
        PushResult(result);
        return result;
    }

    if (compositePublicInputMutation && compositePublicInput.retailRoot) {
        const auto elapsed = static_cast<double>(TickMs() - receivedAt) / 1000.0;
        compositePublicInput.relativeDelaySeconds = (std::max)(0.0,
            compositePublicInput.relativeDelaySeconds - elapsed);
    }
    auto resolved = gRegistry->Resolve(mutation.entity);
    switch (resolved.status) {
    case WorldRegistryResolveStatus::Resolved: {
        result.entityResolved = static_cast<bool>(resolved.lease);
        if (!result.entityResolved) {
            result.code = WorldMutationResultCode::NativeFault;
            gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        }
        gResolvedDispatchCount.fetch_add(1U, std::memory_order_acq_rel);

        // v12.4 probe remains a strict resolve-only no-op.
        if (mutation.operation == kWorldNetworkProbeOperation) {
            result.dispatched = true;
            break;
        }

        if (compositePublicInputMutation) {
            const auto interactionResult =
                world_interactions::ExecuteCompositePublicInput(
                    compositePublicInput, mutation.entity, resolved.lease, *gRegistry);
            switch (interactionResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = interactionResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = compositePublicInput.retailRoot ? WorldMutationResultCode::QueueRejected
                    : WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (observedCharacterCallbackEdge) {
            WorldRegistryResolveResult sourceResolved{};
            WorldRegistryResolveResult callbackResolved{};
            const WorldEntityLease* sourceLease = nullptr;
            const WorldEntityLease* callbackLease = nullptr;

            if (characterCallbackEdge.sourceEntity == mutation.entity) {
                sourceLease = &resolved.lease;
            } else {
                sourceResolved = gRegistry->Resolve(characterCallbackEdge.sourceEntity);
                if (sourceResolved.status == WorldRegistryResolveStatus::Resolved && sourceResolved.lease) {
                    sourceLease = &sourceResolved.lease;
                }
            }
            if (characterCallbackEdge.callbackEntity == mutation.entity) {
                callbackLease = &resolved.lease;
            } else if (sourceLease != nullptr &&
                characterCallbackEdge.callbackEntity == characterCallbackEdge.sourceEntity) {
                callbackLease = sourceLease;
            } else {
                callbackResolved = gRegistry->Resolve(characterCallbackEdge.callbackEntity);
                if (callbackResolved.status == WorldRegistryResolveStatus::Resolved && callbackResolved.lease) {
                    callbackLease = &callbackResolved.lease;
                }
            }
            if (sourceLease == nullptr || callbackLease == nullptr) {
                const auto sourcePending = sourceResolved.status == WorldRegistryResolveStatus::PendingUntilAvailable ||
                    sourceResolved.status == WorldRegistryResolveStatus::RuntimeUnavailable;
                const auto callbackPending = callbackResolved.status == WorldRegistryResolveStatus::PendingUntilAvailable ||
                    callbackResolved.status == WorldRegistryResolveStatus::RuntimeUnavailable;
                if (sourcePending || callbackPending) {
                    result.code = WorldMutationResultCode::RuntimePending;
                    gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                } else {
                    result.code = WorldMutationResultCode::EntityRejected;
                    gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            }
            const auto callbackResult = world_interactions::ExecuteCharacterCallbackInputEdge(
                characterCallbackEdge, resolved.lease, *sourceLease, *callbackLease);
            switch (callbackResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = callbackResult.queued;
                if (result.dispatched) gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (observedCallbackInteractionEdge) {
            const WorldEntityLease* sourceLease = nullptr;
            WorldRegistryResolveResult sourceResolved{};
            if (callbackEdge.sourceEntity == mutation.entity) {
                sourceLease = &resolved.lease;
            } else {
                sourceResolved = gRegistry->Resolve(callbackEdge.sourceEntity);
                switch (sourceResolved.status) {
                case WorldRegistryResolveStatus::Resolved:
                    if (sourceResolved.lease) {
                        sourceLease = &sourceResolved.lease;
                        break;
                    }
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::PendingUntilAvailable:
                case WorldRegistryResolveStatus::RuntimeUnavailable:
                    result.code = WorldMutationResultCode::RuntimePending;
                    gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::NativeFault:
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::EntityNotInManifest:
                    result.code = WorldMutationResultCode::EntityRejected;
                    gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::GenerationMismatch:
                    result.code = WorldMutationResultCode::GenerationRejected;
                    gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::ManifestNotLoaded:
                default:
                    result.code = WorldMutationResultCode::ManifestRejected;
                    gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                }
            }
            if (sourceLease == nullptr) break;

            const auto callbackResult = world_interactions::ExecuteCallbackInputEdge(
                callbackEdge, resolved.lease, *sourceLease);
            switch (callbackResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = callbackResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (observedLogicSwitchState) {
            const auto stateResult = world_interactions::ExecuteLogicSwitchState(
                logicSwitchState, resolved.key, resolved.lease);
            switch (stateResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = stateResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (observedTypedMessage) {
            // Fix3.9 BuildFix01: remote replay is restricted to the previously
            // proven v12.8 class where both runtime keys belong to manifested
            // WorldEntities. This independently rejects widened/non-World
            // traffic even if a malformed or mixed peer manages to send it.
            const auto sourceWorld =
                gRegistry->FindRuntimeWorldEntityId(typedMessage.sourceRuntimeKey);
            const auto targetWorld =
                gRegistry->FindRuntimeWorldEntityId(typedMessage.targetRuntimeKey);
            const bool rawGateAccepted = sourceWorld.has_value() && targetWorld.has_value() &&
                mutation.entity == *targetWorld && resolved.key == typedMessage.targetRuntimeKey;
            if (!rawGateAccepted) {
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }

            const auto typedResult = world_interactions::ExecuteTypedMessage(
                typedMessage, typedMessageData);
            world_interactions::ObserveRemoteTypedReplayResult(
                typedMessage, typedMessageData.size(), typedResult.status, typedResult.queued);
            switch (typedResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = typedResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (doorAction) {
            const auto doorResult = world_animated::ExecuteDoorAction(
                doorActionPayload, mutation.entity, resolved.lease);
            switch (doorResult.status) {
            case world_animated::ExecuteStatus::Queued:
                result.dispatched = doorResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_animated::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::SuppressedLocalMatch:
                result.dispatched = true;
                gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (animatedInputEdge) {
            const WorldEntityLease* sourceLease = nullptr;
            WorldRegistryResolveResult sourceResolved{};
            if (animatedEdge.sourceEntity == mutation.entity) {
                sourceLease = &resolved.lease;
            } else {
                sourceResolved = gRegistry->Resolve(animatedEdge.sourceEntity);
                switch (sourceResolved.status) {
                case WorldRegistryResolveStatus::Resolved:
                    if (sourceResolved.lease) {
                        sourceLease = &sourceResolved.lease;
                        break;
                    }
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::PendingUntilAvailable:
                case WorldRegistryResolveStatus::RuntimeUnavailable:
                    result.code = WorldMutationResultCode::RuntimePending;
                    gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::NativeFault:
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::EntityNotInManifest:
                    result.code = WorldMutationResultCode::EntityRejected;
                    gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::GenerationMismatch:
                    result.code = WorldMutationResultCode::GenerationRejected;
                    gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::ManifestNotLoaded:
                default:
                    result.code = WorldMutationResultCode::ManifestRejected;
                    gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                }
            }
            if (sourceLease == nullptr) break;

            const auto animatedResult = world_animated::ExecuteAnimatedInputEdge(
                animatedEdge, mutation.entity, resolved.lease, *sourceLease);
            switch (animatedResult.status) {
            case world_animated::ExecuteStatus::Queued:
                result.dispatched = animatedResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_animated::ExecuteStatus::SuppressedLocalMatch:
                result.dispatched = true;
                gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_animated::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (observedInteractionEdge) {
            const WorldEntityLease* sourceLease = nullptr;
            WorldRegistryResolveResult sourceResolved{};
            if (observedEdge.sourceEntity == mutation.entity) {
                sourceLease = &resolved.lease;
            } else {
                sourceResolved = gRegistry->Resolve(observedEdge.sourceEntity);
                switch (sourceResolved.status) {
                case WorldRegistryResolveStatus::Resolved:
                    if (sourceResolved.lease) {
                        sourceLease = &sourceResolved.lease;
                        break;
                    }
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::PendingUntilAvailable:
                case WorldRegistryResolveStatus::RuntimeUnavailable:
                    result.code = WorldMutationResultCode::RuntimePending;
                    gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::NativeFault:
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::EntityNotInManifest:
                    result.code = WorldMutationResultCode::EntityRejected;
                    gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::GenerationMismatch:
                    result.code = WorldMutationResultCode::GenerationRejected;
                    gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::ManifestNotLoaded:
                default:
                    result.code = WorldMutationResultCode::ManifestRejected;
                    gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                }
            }
            if (sourceLease == nullptr) break;

            const auto interactionResult = world_interactions::Execute(
                observedEdge, resolved.lease, *sourceLease);
            switch (interactionResult.status) {
            case world_interactions::ExecuteStatus::Queued:
                result.dispatched = interactionResult.queued;
                if (result.dispatched) {
                    gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
                }
                break;
            case world_interactions::ExecuteStatus::RuntimeUnavailable:
                result.code = WorldMutationResultCode::RuntimePending;
                gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::NativeFault:
                result.code = WorldMutationResultCode::NativeFault;
                gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            case world_interactions::ExecuteStatus::InvalidPayload:
            default:
                result.code = WorldMutationResultCode::EntityRejected;
                gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                break;
            }
            break;
        }

        if (mutation.operation != cathode_primitives::kOperationTriggerInput) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        }

        // TriggerInput is a one-shot CATHODE edge. Never execute it from a
        // replaceable/replayable State or Timeline mutation.
        if (mutation.kind != WorldMutationKind::Event) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        }

        cathode_primitives::TriggerInputRequest request{};
        if (!cathode_primitives::DecodeTriggerInputPayload(payload, request)) {
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        }

        const WorldEntityLease* sourceLease = nullptr;
        WorldRegistryResolveResult sourceResolved{};
        if (request.hasSourceEntity) {
            if (request.sourceEntity == mutation.entity) {
                sourceLease = &resolved.lease;
            } else {
                sourceResolved = gRegistry->Resolve(request.sourceEntity);
                switch (sourceResolved.status) {
                case WorldRegistryResolveStatus::Resolved:
                    if (sourceResolved.lease) {
                        sourceLease = &sourceResolved.lease;
                        break;
                    }
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::PendingUntilAvailable:
                case WorldRegistryResolveStatus::RuntimeUnavailable:
                    result.code = WorldMutationResultCode::RuntimePending;
                    gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::NativeFault:
                    result.code = WorldMutationResultCode::NativeFault;
                    gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::EntityNotInManifest:
                    result.code = WorldMutationResultCode::EntityRejected;
                    gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::GenerationMismatch:
                    result.code = WorldMutationResultCode::GenerationRejected;
                    gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                case WorldRegistryResolveStatus::ManifestNotLoaded:
                default:
                    result.code = WorldMutationResultCode::ManifestRejected;
                    gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
                    break;
                }
                if (sourceLease == nullptr) break;
            }
        }

        const auto primitiveResult = cathode_primitives::Execute(
            mutation.operation,
            payload,
            resolved.lease,
            sourceLease);
        switch (primitiveResult.status) {
        case cathode_primitives::ExecuteStatus::Queued:
            result.dispatched = primitiveResult.queued;
            if (result.dispatched) {
                gAppliedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
            }
            break;
        case cathode_primitives::ExecuteStatus::RuntimeUnavailable:
            result.code = WorldMutationResultCode::RuntimePending;
            gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        case cathode_primitives::ExecuteStatus::NativeFault:
            result.code = WorldMutationResultCode::NativeFault;
            gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        case cathode_primitives::ExecuteStatus::UnsupportedOperation:
        case cathode_primitives::ExecuteStatus::InvalidPayload:
        default:
            result.code = WorldMutationResultCode::EntityRejected;
            gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
            break;
        }
        break;
    }
    case WorldRegistryResolveStatus::PendingUntilAvailable:
    case WorldRegistryResolveStatus::RuntimeUnavailable:
        result.code = WorldMutationResultCode::RuntimePending;
        gRuntimePendingCount.fetch_add(1U, std::memory_order_acq_rel);
        break;
    case WorldRegistryResolveStatus::NativeFault:
        result.code = WorldMutationResultCode::NativeFault;
        gNativeFaultCount.fetch_add(1U, std::memory_order_acq_rel);
        break;
    case WorldRegistryResolveStatus::EntityNotInManifest:
        result.code = WorldMutationResultCode::EntityRejected;
        gEntityRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
        break;
    case WorldRegistryResolveStatus::GenerationMismatch:
        result.code = WorldMutationResultCode::GenerationRejected;
        gGenerationRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
        break;
    case WorldRegistryResolveStatus::ManifestNotLoaded:
    default:
        result.code = WorldMutationResultCode::ManifestRejected;
        gManifestRejectedCount.fetch_add(1U, std::memory_order_acq_rel);
        break;
    }
    PushResult(result);
    return result;
}

[[nodiscard]] bool QueueDeferredRemoteApply(
    const WorldMutation& mutation,
    const world_interactions::CompositePublicInputEvent& root,
    const std::uint64_t receivedAt,
    const bool suppressLocalEcho) noexcept {
    std::lock_guard lock(gDeferredRemoteApplyMutex);
    const auto discardPrefix = [&](const bool reportFailure) {
        const auto tx = gIngressRoot.authorityTransaction;
        while (tx != 0U && !gDeferredRemoteApplies.empty() &&
            gDeferredRemoteApplies.back().root.authorityTransaction == tx) {
            if (reportFailure)
                PushResult(Rejected(gDeferredRemoteApplies.back().mutation, WorldMutationResultCode::QueueRejected));
            gDeferredRemoteApplies.pop_back();
        }
        gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
    };
    if (root.rootOrdinal == 0U) {
        // A fresh ordinal-0 fragment may be a safe retransmission after a
        // disconnect. The old prefix never became a complete segment and has
        // never crossed gameplay, so replace it without emitting stale failure
        // results for the superseded attempt.
        if (gReservedRemoteRoots != 0U) { discardPrefix(false); gSequenceGaps.fetch_add(1U, std::memory_order_relaxed); }
        if (gDeferredRemoteApplies.size() + gInFlightRemoteRoots + root.rootCount > kDeferredRemoteApplyCapacity) {
            gDeferredOverflowCount.fetch_add(1U, std::memory_order_relaxed);
            PushResult(Rejected(mutation, WorldMutationResultCode::QueueRejected));
            return false;
        }
        gReservedRemoteRoots = root.rootCount;
        gIngressRoot = root; gIngressPeer = mutation.sourcePeer; gIngressNextOrdinal = 0U;
    }
    if (gReservedRemoteRoots == 0U || root.authorityTransaction != gIngressRoot.authorityTransaction ||
        root.causalSerial != gIngressRoot.causalSerial || root.segmentSerial != gIngressRoot.segmentSerial ||
        root.rootCount != gIngressRoot.rootCount || root.rootOrdinal != gIngressNextOrdinal ||
        mutation.sourcePeer != gIngressPeer) {
        discardPrefix(true);
        gSequenceGaps.fetch_add(1U, std::memory_order_relaxed);
        PushResult(Rejected(mutation, WorldMutationResultCode::QueueRejected));
        return false;
    }
    DeferredRemoteApply pending{};
    pending.mutation = mutation;
    pending.root = root;
    pending.receivedAt = receivedAt;
    pending.expiresAt = receivedAt + kDeferredRemoteApplyLifetimeMs;
    pending.nextRetryAt = receivedAt;
    pending.suppressLocalEcho = suppressLocalEcho;
    try { gDeferredRemoteApplies.push_back(std::move(pending)); }
    catch (...) { discardPrefix(true); PushResult(Rejected(mutation, WorldMutationResultCode::QueueRejected)); return false; }
    --gReservedRemoteRoots;
    ++gIngressNextOrdinal;
    if (gReservedRemoteRoots == 0U) { gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U; }
    gDeferredQueuedCount.fetch_add(1U, std::memory_order_acq_rel);
    return true;
}

// Seal and publish the selected prefix (zero selects all staged ROOTs) while
// the caller holds gOutboundSubmitMutex. True makes that prefix visible to
// the sole outbound-ring consumer; false means none of the staged ROOTs remain
// reserved or staged. This is the synchronous publication barrier used by
// AwaitAuthority so a caller never suppresses retail on a merely reserved ROOT.
[[nodiscard]] bool SealAndPublishStagedRootsLocked(std::size_t prefixCount = 0U) noexcept {
    if (gStagedRoots.empty()) return true;
    const auto count = prefixCount == 0U ? gStagedRoots.size() : (std::min)(prefixCount, gStagedRoots.size());
    bool publishable = true;
    for (std::size_t start = 0U; start < count;) {
        std::size_t end = start + 1U;
        while (end < count && end - start < world_interactions::kMaximumTransactionRoots &&
            gStagedRoots[end].causalSerial == gStagedRoots[start].causalSerial &&
            gStagedRoots[end].intent.levelGeneration == gStagedRoots[start].intent.levelGeneration)
            ++end;
        const auto serial = gSegmentSerial.fetch_add(1U, std::memory_order_relaxed);
        for (std::size_t i = start; i < end; ++i) {
            auto& intent = gStagedRoots[i].intent;
            world_interactions::CompositePublicInputEvent root{};
            if (gGenerations == nullptr || intent.levelGeneration != gGenerations->CurrentGeneration() ||
                !world_interactions::DecodeCompositePublicInputPayload(
                    std::span<const std::byte>(intent.payload.bytes.data(), intent.payload.size), root)) {
                publishable = false; break;
            }
            root.segmentSerial = serial;
            root.rootOrdinal = static_cast<std::uint16_t>(i - start);
            root.rootCount = static_cast<std::uint16_t>(end - start);
            root.authorityTransaction = 0U;
            if (!world_interactions::EncodeCompositePublicInputPayload(root, intent.payload)) {
                publishable = false; break;
            }
            intent.clientSequence = gLocalIntentSequence.fetch_add(1U, std::memory_order_acq_rel);
        }
        if (!publishable) break;
        gSealedSegments.fetch_add(1U, std::memory_order_relaxed);
        start = end;
    }
    const bool published = publishable &&
        gOutboundIntents.PublishReserved(gStagedRoots.begin(), gStagedRoots.begin() + count);
    if (!published) {
        for (auto& staged : gStagedRoots) {
            gOutboundIntents.Cancel(staged.ticket);
            if (staged.intent.clientSequence == 0U) staged.intent.clientSequence = 1U;
            static_cast<void>(world_interactions::DiscardPublicRootIntent(staged.intent));
        }
        // Every staged entry is a fully certified public ROOT. Non-Await ROOTs
        // may already have executed retail; Await ROOTs will fall back to retail
        // when SubmitLocalIntent returns false. Either way, silently continuing
        // multiplayer after losing the authoritative publication would be unsafe.
        MarkCausalRecoveryRequired();
        gQueueRejectedCount.fetch_add(gStagedRoots.size(), std::memory_order_relaxed);
    }
    if (published) gStagedRoots.erase(gStagedRoots.begin(), gStagedRoots.begin() + count);
    else gStagedRoots.clear();
    TouchVersion();
    return published;
}

} // namespace

bool Initialize(
    LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher,
    WorldEntityRegistry& registry,
    std::string& error) noexcept {
    error.clear();
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }

    gGenerations = &generations;
    gDispatcher = &dispatcher;
    gRegistry = &registry;
    const auto generation = generations.CurrentGeneration();
    gLevelGenerationAtInitialize.store(generation, std::memory_order_release);
    gInitializationCount.fetch_add(1U, std::memory_order_acq_rel);
    ResetForGeneration(generation);
    TouchVersion();
    return true;
}

bool Initialized() noexcept {
    return gInitialized.load(std::memory_order_acquire);
}

RuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    RuntimeDiagnostics result{};
    result.initialized = Initialized();
    result.version = gVersion.load(std::memory_order_acquire);
    result.initializationCount = gInitializationCount.load(std::memory_order_acquire);
    result.shutdownCount = gShutdownCount.load(std::memory_order_acquire);
    result.levelGenerationAtInitialize =
        gLevelGenerationAtInitialize.load(std::memory_order_acquire);
    result.environmentHookCount = 0U;
    result.pollingPassCount = 0U;
    result.queuedMutationCount = gDispatcher == nullptr ? 0U : gDispatcher->PendingCount();
    result.observedMutationCount = 0U;
    result.appliedMutationCount = gAppliedMutationCount.load(std::memory_order_acquire);
    result.outboundIntentCount = gOutboundIntentCount.load(std::memory_order_acquire);
    result.outboundPendingCount = gOutboundIntents.Size();
    result.outboundReservedCount = gOutboundIntents.Reserved();
    result.outboundCapacity = kIntentCapacity;
    result.outboundHighWater = gOutboundIntents.HighWater();
    result.outboundCapacityRejected = gOutboundIntents.Rejected();
    result.outboundPoppedCount = gOutboundPoppedCount.load(std::memory_order_acquire);
    result.echoSuppressedCount = gEchoSuppressedCount.load(std::memory_order_acquire);
    result.receivedMutationCount = gReceivedMutationCount.load(std::memory_order_acquire);
    result.acceptedMutationCount = gAcceptedMutationCount.load(std::memory_order_acquire);
    result.duplicateRejectedCount = gDuplicateRejectedCount.load(std::memory_order_acquire);
    result.staleRejectedCount = gStaleRejectedCount.load(std::memory_order_acquire);
    result.generationRejectedCount = gGenerationRejectedCount.load(std::memory_order_acquire);
    result.manifestRejectedCount = gManifestRejectedCount.load(std::memory_order_acquire);
    result.entityRejectedCount = gEntityRejectedCount.load(std::memory_order_acquire);
    result.queueRejectedCount = gQueueRejectedCount.load(std::memory_order_acquire);
    result.resolvedDispatchCount = gResolvedDispatchCount.load(std::memory_order_acquire);
    result.runtimePendingCount = gRuntimePendingCount.load(std::memory_order_acquire);
    result.nativeFaultCount = gNativeFaultCount.load(std::memory_order_acquire);
    result.resetCount = gResetCount.load(std::memory_order_acquire);
    {
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        result.deferredRemoteApplyCount = gDeferredRemoteApplies.size();
    }
    result.deferredQueuedCount = gDeferredQueuedCount.load(std::memory_order_acquire);
    result.deferredRetryCount = gDeferredRetryCount.load(std::memory_order_acquire);
    result.deferredExecutedCount = gDeferredExecutedCount.load(std::memory_order_acquire);
    result.deferredExpiredCount = gDeferredExpiredCount.load(std::memory_order_acquire);
    result.deferredOverflowCount = gDeferredOverflowCount.load(std::memory_order_acquire);
    result.causalSegmentsSealed = gSealedSegments.load(std::memory_order_relaxed);
    result.causalSegmentsApplied = gAppliedSegments.load(std::memory_order_relaxed);
    result.causalSegmentsFailed = gFailedSegments.load(std::memory_order_relaxed);
    result.causalSegmentsSkipped = gSkippedSegments.load(std::memory_order_relaxed);
    result.causalSequenceGaps = gSequenceGaps.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(gOrderingMutex);
        result.causalLastCommittedSequence = gLastCommittedSequence;
        result.causalRecoveryRequired = gCausalRecoveryRequired;
        result.causalDivergedAuthorityTransaction = gDivergedAuthorityTransaction;
    }
    result.orderedCarrierTransactionCount =
        gOrderedCarrierTransactionCount.load(std::memory_order_acquire);
    result.orderedCarrierMemberCount =
        gOrderedCarrierMemberCount.load(std::memory_order_acquire);
    result.orderedCarrierCommitCount =
        gOrderedCarrierCommitCount.load(std::memory_order_acquire);
    result.orderedCarrierFailureCount =
        gOrderedCarrierFailureCount.load(std::memory_order_acquire);
    const auto semantic = semantic_reconciliation::DiagnosticsSnapshot();
    result.semanticProbeCount = semantic.probeCount;
    result.semanticContractedProbeCount = semantic.contractedProbeCount;
    result.semanticUnknownContractCount = semantic.unknownContractCount;
    result.semanticEventContractCount = semantic.eventContractCount;
    result.semanticStateContractCount = semantic.stateContractCount;
    result.semanticStateTransitionContractCount = semantic.stateTransitionContractCount;
    result.semanticReaderResolvedCount = semantic.readerResolvedCount;
    result.semanticReaderRuntimeUnavailableCount = semantic.readerRuntimeUnavailableCount;
    result.semanticReaderUnsupportedCount = semantic.readerUnsupportedCount;
    result.semanticReaderInvalidStateCount = semantic.readerInvalidStateCount;
    result.semanticReaderNativeFaultCount = semantic.readerNativeFaultCount;
    result.semanticCandidateApplyCount = semantic.candidateApplyCount;
    result.semanticCandidateSkipCount = semantic.candidateSkipCount;
    result.semanticCandidateMismatchCount = semantic.candidateMismatchCount;
    result.semanticCandidateAlreadyDesiredTransitionUnprovenCount =
        semantic.candidateAlreadyDesiredTransitionUnprovenCount;
    result.semanticCandidateUnknownCount = semantic.candidateUnknownCount;
    result.semanticActionLegacyApplyCount = semantic.actionLegacyApplyCount;
    result.semanticActionApplyCount = semantic.actionApplyCount;
    result.semanticActionSkipCount = semantic.actionSkipCount;
    result.semanticActionRetryCount = semantic.actionRetryCount;
    result.semanticActionRejectCount = semantic.actionRejectCount;
    result.semanticActionTransitionUnprovenRejectCount =
        semantic.actionTransitionUnprovenRejectCount;
    result.semanticActionStatefulMultiRootRejectCount =
        semantic.actionStatefulMultiRootRejectCount;
    result.semanticActionCarrierTransitionApplyCount =
        semantic.actionCarrierTransitionApplyCount;
    result.semanticTraceWriteCount = semantic.traceWriteCount;
    result.semanticHasLast = semantic.hasLast;
    if (semantic.hasLast) {
        result.semanticLastAuthoritySequence = semantic.last.authoritySequence;
        result.semanticLastObservationId = semantic.last.observationId;
        result.semanticLastAuthorityTransaction = semantic.last.authorityTransaction;
        result.semanticLastApplyBoundaryHigh = semantic.last.applyBoundaryEntity.high;
        result.semanticLastApplyBoundaryLow = semantic.last.applyBoundaryEntity.low;
        result.semanticLastApplyBoundaryOperation = semantic.last.applyBoundaryOperation;
        result.semanticLastTargetHigh = semantic.last.semanticTargetEntity.high;
        result.semanticLastTargetLow = semantic.last.semanticTargetEntity.low;
        result.semanticLastTargetPrimitiveType = semantic.last.semanticTargetPrimitiveType;
        result.semanticLastOperation = semantic.last.semanticOperation;
        result.semanticLastRootKind = semantic.last.rootKind;
        result.semanticLastContract = static_cast<std::uint8_t>(semantic.last.contractKind);
        result.semanticLastReadStatus = static_cast<std::uint8_t>(semantic.last.readStatus);
        result.semanticLastCandidateDecision = static_cast<std::uint8_t>(semantic.last.candidateDecision);
        result.semanticLastAction = static_cast<std::uint8_t>(semantic.last.action);
        result.semanticLastDesiredKnown = semantic.last.desiredKnown;
        result.semanticLastDesired = semantic.last.desired;
        result.semanticLastExpectedBeforeKnown = semantic.last.expectedBeforeKnown;
        result.semanticLastExpectedBefore = semantic.last.expectedBefore;
        result.semanticLastLocalBeforeKnown = semantic.last.localBeforeKnown;
        result.semanticLastLocalBefore = semantic.last.localBefore;
        result.semanticLastCarrierBoundary = semantic.last.carrierBoundary;
    }
    result.controlledRemoteApplyProbeOnly = false;
    result.controlledRemoteApplyFirstInteraction = false;
    {
        std::lock_guard lock(gControlledRemoteApplyMutex);
        result.controlledRemoteApplyArmed = gControlledRemoteApplyArmed;
        result.controlledRemoteApplyTargetHigh = gControlledRemoteApplyTarget.high;
        result.controlledRemoteApplyTargetLow = gControlledRemoteApplyTarget.low;
        result.controlledRemoteApplyTargetRootCount = gControlledRemoteApplyRootCount;
        result.controlledRemoteApplyLastSelectedInput = gControlledRemoteApplyLastSelectedInput;
        result.controlledRemoteApplyLastSelectedTargetHigh = gControlledRemoteApplyLastSelectedTarget.high;
        result.controlledRemoteApplyLastSelectedTargetLow = gControlledRemoteApplyLastSelectedTarget.low;
        result.controlledRemoteApplyLastSelectedSequence = gControlledRemoteApplyLastSelectedSequence;
    }
    result.controlledRemoteApplyArmCount = gControlledRemoteApplyArmCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyGameplayCandidateCount = gControlledRemoteApplyGameplayCandidateCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyGameplaySelectedCount = gControlledRemoteApplyGameplaySelectedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyUnsafeContextSuppressedCount = gControlledRemoteApplyUnsafeContextSuppressedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyNotArmedSuppressedCount = gControlledRemoteApplyNotArmedSuppressedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyOtherTargetSuppressedCount = gControlledRemoteApplyOtherTargetSuppressedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyCrossTargetSelectedCount = gControlledRemoteApplyCrossTargetSelectedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyBudgetSuppressedCount = gControlledRemoteApplyBudgetSuppressedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplyParsedCount =
        gControlledRemoteApplyParsedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplySelectedCount =
        gControlledRemoteApplySelectedCount.load(std::memory_order_acquire);
    result.controlledRemoteApplySuppressedCount =
        gControlledRemoteApplySuppressedCount.load(std::memory_order_acquire);
    return result;
}

void BeginCausalTurn() noexcept { ++gCausalTurnDepth; }

void EndCausalTurn() noexcept {
    if (gCausalTurnDepth == 0U || --gCausalTurnDepth != 0U || gStagedRoots.empty()) return;
    std::lock_guard submitLock(gOutboundSubmitMutex);
    static_cast<void>(SealAndPublishStagedRootsLocked());
}

bool SubmitLocalIntent(
    const WorldEntityId& entity,
    const WorldMutationKind requestedKind,
    const std::uint32_t operation,
    const std::span<const std::byte> payload) noexcept {
    if (!Initialized() || gGenerations == nullptr || gRegistry == nullptr ||
        !entity.Valid() || !ValidKind(requestedKind) || operation == 0U ||
        payload.size() > kWorldPayloadMaximumBytes) return false;
    if (RemoteApplyActive()) {
        gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
        TouchVersion();
        return false;
    }
    {
        std::lock_guard lock(gOrderingMutex);
        if (gCausalRecoveryRequired) return false;
    }

    const auto registry = gRegistry->ReplicationContext();
    const auto generation = gGenerations->CurrentGeneration();
    if (!registry.manifestLoaded || registry.levelGeneration != generation ||
        generation == 0U || !registry.mapContentId.Valid()) return false;

    std::lock_guard submitLock(gOutboundSubmitMutex);
    WorldIntent intent{};
    intent.manifest = {
        kWorldManifestFormatVersion,
        kWorldManifestCompilerVersion,
        registry.mapContentId};
    intent.entity = entity;
    intent.levelGeneration = static_cast<std::uint32_t>(generation);
    intent.sourcePeer = 0U; // Launcher binds the authenticated lobby peer.
    intent.requestedKind = requestedKind;
    intent.operation = operation;
    intent.payload.size = static_cast<std::uint16_t>(payload.size());
    std::copy(payload.begin(), payload.end(), intent.payload.bytes.begin());
    if (operation == world_interactions::kOperationCompositePublicInput && gCausalTurnDepth != 0U) {
        world_interactions::CompositePublicInputEvent root{};
        if (!world_interactions::DecodeCompositePublicInputPayload(payload, root) || !root.retailRoot) return false;
        const auto ticket = gOutboundIntents.Reserve();
        if (ticket == 0U) { MarkCausalRecoveryRequired(); return false; }
        try { gStagedRoots.push_back(StagedRoot{intent, ticket, root.causalSerial}); }
        catch (...) { gOutboundIntents.Cancel(ticket); MarkCausalRecoveryRequired(); return false; }

        const auto stablePrefix = StableCausalPublicationPrefix(gStagedRoots,
            world_interactions::kMaximumTransactionRoots, [](const auto& a, const auto& b) {
                return a.causalSerial == b.causalSerial && a.intent.levelGeneration == b.intent.levelGeneration;
            });
        if (stablePrefix != 0U && !SealAndPublishStagedRootsLocked(stablePrefix)) return false;

        // AwaitAuthority is a synchronous takeover promise. Do not report
        // success while this ROOT is merely reserved/staged: seal and publish
        // the complete prefix now, while the native invocation is still on the
        // stack and a false return can preserve retail execution (Step 3).
        if ((root.flags & world_actions::kPublicRootFlagAwaitAuthority) != 0U &&
            !SealAndPublishStagedRootsLocked()) {
            return false;
        }
    } else {
        if (operation == world_interactions::kOperationCompositePublicInput) {
            world_interactions::CompositePublicInputEvent root{};
            if (!world_interactions::DecodeCompositePublicInputPayload(payload, root) || !root.retailRoot) return false;
            root.segmentSerial = gSegmentSerial.fetch_add(1U, std::memory_order_relaxed);
            root.rootOrdinal = 0U; root.rootCount = 1U; root.authorityTransaction = 0U;
            if (!world_interactions::EncodeCompositePublicInputPayload(root, intent.payload)) {
                MarkCausalRecoveryRequired();
                return false;
            }
        }
        intent.clientSequence = gLocalIntentSequence.fetch_add(1U, std::memory_order_acq_rel);
        if (!gOutboundIntents.Push(intent)) {
            if (operation == world_interactions::kOperationCompositePublicInput)
                MarkCausalRecoveryRequired();
            return false;
        }
    }
    gOutboundIntentCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

bool TryPopOutboundIntent(WorldIntent& output) noexcept {
    if (!gOutboundIntents.Pop(output)) return false;
    gOutboundPoppedCount.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void MarkOutboundIntentTransportFailure(const WorldIntent& intent) noexcept {
    if (intent.operation != world_interactions::kOperationCompositePublicInput) return;
    world_interactions::CompositePublicInputEvent root{};
    if (!world_interactions::DecodeCompositePublicInputPayload(
            std::span<const std::byte>(intent.payload.bytes.data(), intent.payload.size), root)
        || !root.retailRoot)
        return;
    // The worker already removed this certified public ROOT from the native
    // ring. If IPC transport now fails, an optimistic ROOT may have executed
    // locally and an Await ROOT may already have surrendered retail. Neither is
    // safe to forget, so enter fail-stop until a fresh world/generation exists.
    MarkCausalRecoveryRequired();
    gQueueRejectedCount.fetch_add(1U, std::memory_order_relaxed);
    TouchVersion();
}

bool ReceiveAuthoritativeMutation(const WorldMutation& mutation) noexcept {
    gReceivedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
    if (!Initialized() || gGenerations == nullptr || gDispatcher == nullptr ||
        gRegistry == nullptr || !ValidMutation(mutation)) {
        CountRejection(WorldMutationResultCode::EntityRejected);
        PushResult(Rejected(mutation, WorldMutationResultCode::EntityRejected));
        return false;
    }

    const auto currentGeneration = gGenerations->CurrentGeneration();
    if (mutation.levelGeneration != currentGeneration) {
        CountRejection(WorldMutationResultCode::GenerationRejected);
        PushResult(Rejected(mutation, WorldMutationResultCode::GenerationRejected));
        return false;
    }

    const auto registry = gRegistry->ReplicationContext();
    if (!registry.manifestLoaded || registry.levelGeneration != mutation.levelGeneration ||
        registry.mapContentId != mutation.manifest.mapContentId ||
        !mutation.manifest.Valid()) {
        CountRejection(WorldMutationResultCode::ManifestRejected);
        PushResult(Rejected(mutation, WorldMutationResultCode::ManifestRejected));
        return false;
    }

    world_interactions::CompositePublicInputEvent receivedRoot{};
    const bool publicRoot = mutation.operation == world_interactions::kOperationCompositePublicInput;
    PublicCauseKey causeKey{};
    if (publicRoot) {
        if (mutation.kind != WorldMutationKind::Event ||
            !world_interactions::DecodeCompositePublicInputPayload(
                std::span<const std::byte>(mutation.payload.bytes.data(), mutation.payload.size), receivedRoot) ||
            (world_actions::Installed() && !receivedRoot.retailRoot)) {
            CountRejection(WorldMutationResultCode::EntityRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::EntityRejected));
            return false;
        }
        if (receivedRoot.retailRoot && (receivedRoot.authorityTransaction == 0U ||
            mutation.sequence != receivedRoot.authorityTransaction + receivedRoot.rootOrdinal)) {
            CountRejection(WorldMutationResultCode::EntityRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::EntityRejected));
            return false;
        }
        causeKey = {mutation.sourcePeer, receivedRoot.observationId};
        std::lock_guard lock(gOrderingMutex);
        if (gCausalRecoveryRequired) {
            CountRejection(WorldMutationResultCode::QueueRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::QueueRejected));
            return false;
        }
        // Durable duplicate state is commit-level only. A retransmission at or
        // below the committed checkpoint is an idempotent no-op acknowledged as
        // success so a reconnected Launcher can rebuild the same checkpoint
        // without re-entering CATHODE. Uncommitted causes are never remembered.
        if (mutation.sequence <= gLastCommittedSequence) {
            PushResult({mutation.sequence, WorldMutationResultCode::Accepted, true, true, mutation.nativeDispatchId});
            return true;
        }
        if (gPublicCauses.contains(causeKey)) {
            CountRejection(WorldMutationResultCode::DuplicateRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::DuplicateRejected));
            return false;
        }
    } else {
        std::lock_guard lock(gOrderingMutex);
        if (gCausalRecoveryRequired) {
            CountRejection(WorldMutationResultCode::QueueRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::QueueRejected));
            return false;
        }
        if (mutation.sequence <= gLastCommittedSequence) {
            PushResult({mutation.sequence, WorldMutationResultCode::Accepted, true, true, mutation.nativeDispatchId});
            return true;
        }
        if (mutation.kind == WorldMutationKind::State ||
            mutation.kind == WorldMutationKind::Timeline) {
            const StateKey key{mutation.entity, mutation.operation, mutation.kind};
            const auto found = gStateRevisions.find(key);
            if (found != gStateRevisions.end() && mutation.revision <= found->second) {
                CountRejection(WorldMutationResultCode::StaleRejected);
                PushResult(Rejected(mutation, WorldMutationResultCode::StaleRejected));
                return false;
            }
        }
    }

    const auto receivedAt = TickMs();

    // 0.11.4.77 live generic WS3:
    // every authoritative public ROOT that survived the static Actor -> World
    // boundary is admitted to the native Remote Apply pipeline. ProbeDirect is
    // now only a diagnostic witness and is no longer required to arm gameplay.
    // The receiver deliberately relies on generic ROOT v9 validation and
    // runtime dependency resolution rather than object-specific admission.
    if (publicRoot && receivedRoot.retailRoot) {
        if (!world_actions::PublicRootManifestAdmissionAllowed(*gRegistry, mutation.entity, receivedRoot)) {
            CountRejection(WorldMutationResultCode::EntityRejected);
            PushResult(Rejected(mutation, WorldMutationResultCode::EntityRejected));
            return false;
        }

        gControlledRemoteApplyParsedCount.fetch_add(1U, std::memory_order_acq_rel);
        gControlledRemoteApplySelectedCount.fetch_add(1U, std::memory_order_acq_rel);

        // Selected ROOTs never occupy GameThreadDispatcher while waiting for
        // runtime entities/context. They live in a tiny, independent FIFO that is
        // inspected only from proven engine-thread hooks. This prevents one missing
        // dependency from holding a dispatcher slot for seconds and avoids a ROOT
        // burst starving input/player commands.
        const bool suppressLocalEcho =
            (receivedRoot.flags & world_actions::kPublicRootFlagAwaitAuthority) == 0U &&
            (world_actions::IsLocalPeer(mutation.sourcePeer) ||
                world_interactions::HasLocalObservation(receivedRoot.observationId));
        if (!QueueDeferredRemoteApply(
                mutation, receivedRoot, receivedAt, suppressLocalEcho)) {
            CountRejection(WorldMutationResultCode::QueueRejected);
            TouchVersion();
            return false;
        }
        // Received/assembled is not committed. Sequence/cause durability moves
        // to PumpDeferredRemoteApplyOnEngineThread after the WHOLE segment has
        // completed successfully.
        TouchVersion();
        return true;
    }

    GameThreadCommand command{};
    command.requestId = kWorldMutationRequestBase ^ mutation.sequence;
    command.levelGeneration = mutation.levelGeneration;
    command.expiresAtTickMs = receivedAt + kDispatchLifetimeMs;
    command.kind = GameThreadCommandKind::WorldMutation;
    command.execute = [mutation, receivedAt] {
        const auto result = ExecuteMutationOnEngineThread(mutation, receivedAt);
        std::lock_guard lock(gOrderingMutex);
        gLastCommittedSequence = (std::max)(gLastCommittedSequence, mutation.sequence);
        if (result.code != WorldMutationResultCode::Accepted || !result.dispatched) {
            gSkippedSegments.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        if (mutation.kind == WorldMutationKind::State || mutation.kind == WorldMutationKind::Timeline)
            gStateRevisions[{mutation.entity, mutation.operation, mutation.kind}] = mutation.revision;
        gAcceptedMutationCount.fetch_add(1U, std::memory_order_acq_rel);
    };
    command.completion = [sequence = mutation.sequence, nativeDispatchId = mutation.nativeDispatchId](
        const DispatchCompletion completion) {
        if (completion == DispatchCompletion::Executed) return;
        const auto code = completion == DispatchCompletion::GenerationInvalidated
            ? WorldMutationResultCode::GenerationRejected
            : WorldMutationResultCode::QueueRejected;
        CountRejection(code);
        PushResult({sequence, code, false, false, nativeDispatchId});
    };

    const auto queued = gDispatcher->Enqueue(std::move(command));
    if (queued != DispatchEnqueueResult::Queued) {
        const auto code = queued == DispatchEnqueueResult::GenerationMismatch
            ? WorldMutationResultCode::GenerationRejected
            : WorldMutationResultCode::QueueRejected;
        CountRejection(code);
        PushResult(Rejected(mutation, code));
        return false;
    }

    // Queue admission is not commit. The engine-thread execute closure above
    // advances ordering/revision durability only after the native mutation has
    // actually returned Accepted+Dispatched.
    TouchVersion();
    return true;
}

static std::size_t PumpOneDeferredRemoteTransaction(const std::size_t budget) noexcept {
    if (!Initialized() || gDispatcher == nullptr || gRegistry == nullptr || gGenerations == nullptr ||
        budget == 0U ||
        gDispatcher->EngineThreadId() == 0U ||
        gDispatcher->EngineThreadId() != GetCurrentThreadId()) {
        return 0U;
    }

    if (gPumpingCausalSegment) return 0U;
    struct PumpGuard { PumpGuard() { gPumpingCausalSegment = true; } ~PumpGuard() { gPumpingCausalSegment = false; } } guard;
    std::vector<DeferredRemoteApply> segment;
    struct InFlightGuard {
        std::size_t count{};
        ~InFlightGuard() {
            if (count == 0U) return;
            std::lock_guard lock(gDeferredRemoteApplyMutex);
            gInFlightRemoteRoots -= count;
        }
    } inFlight;
    {
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        if (gDeferredRemoteApplies.empty()) return 0U;
        const auto& first = gDeferredRemoteApplies.front();
        if (first.nextRetryAt > TickMs()) return 0U;
        const auto count = first.root.rootCount;
        if (first.root.rootOrdinal != 0U) {
            // Missing/rejected prefix: never apply a transaction suffix alone.
            gSequenceGaps.fetch_add(1U, std::memory_order_relaxed);
            PushResult(Rejected(first.mutation, WorldMutationResultCode::QueueRejected));
            gDeferredRemoteApplies.pop_front();
            return 1U;
        }
        if (gDeferredRemoteApplies.size() < count) {
            if (TickMs() >= first.expiresAt) {
                const auto tx = first.root.authorityTransaction;
                while (!gDeferredRemoteApplies.empty() && gDeferredRemoteApplies.front().root.authorityTransaction == tx) {
                    PushResult(Rejected(gDeferredRemoteApplies.front().mutation, WorldMutationResultCode::QueueRejected));
                    gDeferredRemoteApplies.pop_front();
                }
                if (gIngressRoot.authorityTransaction == tx) {
                    gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
                }
                gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
                gDeferredExpiredCount.fetch_add(1U, std::memory_order_relaxed);
            }
            return 0U; // completeness, never an arrival-time grouping rule
        }
        for (std::size_t i = 0U; i < count; ++i) {
            const auto& next = gDeferredRemoteApplies[i];
            if (next.root.authorityTransaction != first.root.authorityTransaction ||
                next.root.causalSerial != first.root.causalSerial || next.root.segmentSerial != first.root.segmentSerial ||
                next.mutation.sourcePeer != first.mutation.sourcePeer || next.root.rootOrdinal != i ||
                next.root.rootCount != count || next.mutation.sequence != first.mutation.sequence + i) {
                gSequenceGaps.fetch_add(1U, std::memory_order_relaxed);
                const auto tx = first.root.authorityTransaction;
                while (!gDeferredRemoteApplies.empty() && gDeferredRemoteApplies.front().root.authorityTransaction == tx) {
                    PushResult(Rejected(gDeferredRemoteApplies.front().mutation, WorldMutationResultCode::QueueRejected));
                    gDeferredRemoteApplies.pop_front();
                }
                if (gIngressRoot.authorityTransaction == tx) {
                    gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
                }
                gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
                return 1U;
            }
        }
        try { segment.reserve(count); }
        catch (...) { return 0U; }
        gInFlightRemoteRoots += count;
        inFlight.count = count;
        for (std::size_t i = 0U; i < count; ++i) {
            segment.push_back(std::move(gDeferredRemoteApplies.front()));
            gDeferredRemoteApplies.pop_front();
        }
    }
    // An unsuccessful transaction is consumed once, without replaying effects
    // which may already have run. Its original failure results remain visible
    // to the Launcher, but the next transaction must not be fenced by the gap.
    // A transient wait relinquishes in-flight ownership and is not settled.
    struct SettlementFence {
        const std::vector<DeferredRemoteApply>& segment;
        const InFlightGuard& inFlight;
        ~SettlementFence() {
            if (inFlight.count == 0U || segment.empty()) return;
            std::lock_guard lock(gOrderingMutex);
            if (!gCausalRecoveryRequired && gGenerations != nullptr &&
                segment.front().mutation.levelGeneration == gGenerations->CurrentGeneration() &&
                gLastCommittedSequence < segment.back().mutation.sequence) {
                gLastCommittedSequence = segment.back().mutation.sequence;
                for (const auto& member : segment)
                    RememberPublicCauseLocked({member.mutation.sourcePeer, member.root.observationId});
                gSkippedSegments.fetch_add(1U, std::memory_order_relaxed);
                TouchVersion();
            }
        }
    } settlementFence{segment, inFlight};
    {
        std::lock_guard lock(gOrderingMutex);
        if (gCausalRecoveryRequired) {
            for (const auto& member : segment)
                PushResult(Rejected(member.mutation, WorldMutationResultCode::QueueRejected));
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            return segment.size();
        }
        if (!segment.empty() && segment.back().mutation.sequence <= gLastCommittedSequence) {
            // Recovery/retransmission of an already committed transaction is an
            // idempotent success. Report Accepted without re-entering CATHODE so
            // the Launcher can rebuild its durable checkpoint after reconnect.
            for (const auto& member : segment)
                PushResult({member.mutation.sequence, WorldMutationResultCode::Accepted, true, true, member.mutation.nativeDispatchId});
            return segment.size();
        }
    }
    const auto now = TickMs();

    // Generation must be proven current before any semantic reader dereferences
    // a stable WorldEntityId into live runtime state.
    for (const auto& pending : segment) {
        if (pending.mutation.levelGeneration == gGenerations->CurrentGeneration()) continue;
        gGenerationRejectedCount.fetch_add(segment.size(), std::memory_order_relaxed);
        for (const auto& member : segment)
            PushResult(Rejected(member.mutation, WorldMutationResultCode::GenerationRejected));
        gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
        return segment.size();
    }

    // Patch 4.3 / Patch 5: a stateful multi-root transaction is allowed only
    // when the ENTIRE remote transaction is structurally proven to consist of
    // WM09 StateTransition carriers (EmittedOutput or high-level CompositeInput).
    // Do this manifest-only pass before
    // the first retail call so an unsafe member can never be discovered after a
    // preceding carrier has already mutated CATHODE state. Local-authority echo
    // transactions stay on the historical path; mixed echo/remote transactions
    // are not eligible for this exception.
    bool orderedMultiRootCarrier = segment.size() > 1U;
    if (orderedMultiRootCarrier) {
        for (const auto& pending : segment) {
            if (pending.suppressLocalEcho ||
                !semantic_reconciliation::IsCertifiedOrderedMultiRootCarrierMember(
                    *gRegistry, pending.mutation.entity, pending.root)) {
                orderedMultiRootCarrier = false;
                break;
            }
        }
    }

    if (orderedMultiRootCarrier) {
        gOrderedCarrierTransactionCount.fetch_add(1U, std::memory_order_relaxed);

        const auto requeueWholeBeforeRetail = [&]() -> std::size_t {
            gDeferredRetryCount.fetch_add(1U, std::memory_order_relaxed);
            segment.front().nextRetryAt = TickMs() + kDeferredRemoteApplyRetryMs;
            std::lock_guard lock(gDeferredRemoteApplyMutex);
            gInFlightRemoteRoots -= inFlight.count;
            inFlight.count = 0U;
            for (auto it = segment.rbegin(); it != segment.rend(); ++it)
                gDeferredRemoteApplies.push_front(std::move(*it));
            return 0U;
        };

        const auto rejectWholeBeforeRetail = [&](const WorldMutationResultCode code) -> std::size_t {
            for (const auto& member : segment) {
                CountRejection(code);
                PushResult(Rejected(member.mutation, code));
            }
            gOrderedCarrierFailureCount.fetch_add(1U, std::memory_order_relaxed);
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            return segment.size();
        };

        const auto skipFromMember = [&](const std::size_t index,
                                             const WorldMutationResultCode code) -> std::size_t {
            CountRejection(code);
            PushResult(Rejected(segment[index].mutation, code));
            for (std::size_t j = index + 1U; j < segment.size(); ++j) {
                CountRejection(WorldMutationResultCode::QueueRejected);
                PushResult(Rejected(segment[j].mutation, WorldMutationResultCode::QueueRejected));
            }
            gOrderedCarrierFailureCount.fetch_add(1U, std::memory_order_relaxed);
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            return segment.size();
        };

        const auto skipAfterExecutedMember = [&](const std::size_t index) -> std::size_t {
            // ExecuteMutationOnEngineThread already emitted the failing result
            // for `index`; only reject the not-yet-executed suffix here.
            for (std::size_t j = index + 1U; j < segment.size(); ++j) {
                CountRejection(WorldMutationResultCode::QueueRejected);
                PushResult(Rejected(segment[j].mutation, WorldMutationResultCode::QueueRejected));
            }
            gOrderedCarrierFailureCount.fetch_add(1U, std::memory_order_relaxed);
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            TouchVersion();
            return segment.size();
        };

        // Runtime dependencies may be checked transaction-wide before retail;
        // this does not read semantic state or reconstruct a NativeContext. It
        // prevents a known-pending later member from being discovered only after
        // an earlier carrier has already executed.
        for (auto& pending : segment) {
            const auto reason = PublicRootPendingReason(pending.mutation, pending.root);
            if (reason == 0U) continue;
            if (TickMs() >= segment.front().expiresAt) {
                gDeferredExpiredCount.fetch_add(segment.size(), std::memory_order_relaxed);
                return rejectWholeBeforeRetail(WorldMutationResultCode::RuntimePending);
            }
            if (!pending.pendingRecorded) {
                pending.pendingRecorded = true;
                world_actions::RecordDependencyPending(
                    pending.root, pending.mutation.entity,
                    pending.mutation.sourcePeer, pending.mutation.sequence, reason);
            }
            return requeueWholeBeforeRetail();
        }

        bool crossedRetail = false;
        for (std::size_t i = 0U; i < segment.size(); ++i) {
            auto& pending = segment[i];

            // Fresh semantic read immediately before this ordinal. This is the
            // core Patch 4.3 invariant: ROOT N observes all retail effects of
            // ROOTs [0..N-1], never a transaction-wide stale LocalBefore snapshot.
            const auto semantic = semantic_reconciliation::EvaluateBeforePrepare(
                *gRegistry, pending.mutation.entity, pending.root, pending.mutation.sequence);
            gOrderedCarrierMemberCount.fetch_add(1U, std::memory_order_relaxed);

            if (semantic.action == semantic_reconciliation::ReconciliationAction::RetryRuntimeUnavailable) {
                if (!crossedRetail) {
                    if (TickMs() >= segment.front().expiresAt) {
                        gDeferredExpiredCount.fetch_add(segment.size(), std::memory_order_relaxed);
                        return rejectWholeBeforeRetail(WorldMutationResultCode::RuntimePending);
                    }
                    return requeueWholeBeforeRetail();
                }
                return skipFromMember(i, WorldMutationResultCode::RuntimePending);
            }

            WorldMutationResultCode semanticFailure = WorldMutationResultCode::EntityRejected;
            bool semanticRejected = false;
            switch (semantic.action) {
            case semantic_reconciliation::ReconciliationAction::ApplyCarrierTransition:
                break;
            case semantic_reconciliation::ReconciliationAction::RejectNativeFault:
                semanticFailure = WorldMutationResultCode::NativeFault;
                semanticRejected = true;
                break;
            case semantic_reconciliation::ReconciliationAction::RejectMismatch:
            case semantic_reconciliation::ReconciliationAction::RejectTransitionUnproven:
            case semantic_reconciliation::ReconciliationAction::RejectReaderUnsupported:
            case semantic_reconciliation::ReconciliationAction::RejectInvalidState:
            case semantic_reconciliation::ReconciliationAction::RejectStatefulMultiRoot:
            case semantic_reconciliation::ReconciliationAction::LegacyApply:
            case semantic_reconciliation::ReconciliationAction::ApplyRequired:
            case semantic_reconciliation::ReconciliationAction::SkipAlreadySatisfied:
            default:
                // Structural certification says this member is a carrier. Any
                // action other than ApplyCarrierTransition is therefore a
                // semantic invariant failure and remains fail-closed.
                semanticRejected = true;
                break;
            }
            if (semanticRejected) {
                if (!crossedRetail)
                    return rejectWholeBeforeRetail(semanticFailure);
                return skipFromMember(i, semanticFailure);
            }

            // Re-check availability after earlier carriers have executed. The
            // transaction-wide preflight above prevents known pending roots;
            // this second check detects runtime invalidation CAUSED by an earlier
            // member. Once retail has been crossed, discard the remaining members;
            // never replay the transaction.
            const auto reason = PublicRootPendingReason(pending.mutation, pending.root);
            if (reason != 0U) {
                if (!crossedRetail) {
                    if (TickMs() >= segment.front().expiresAt) {
                        gDeferredExpiredCount.fetch_add(segment.size(), std::memory_order_relaxed);
                        return rejectWholeBeforeRetail(WorldMutationResultCode::RuntimePending);
                    }
                    if (!pending.pendingRecorded) {
                        pending.pendingRecorded = true;
                        world_actions::RecordDependencyPending(
                            pending.root, pending.mutation.entity,
                            pending.mutation.sourcePeer, pending.mutation.sequence, reason);
                    }
                    return requeueWholeBeforeRetail();
                }
                return skipFromMember(i, WorldMutationResultCode::RuntimePending);
            }

            std::shared_ptr<world_actions::PreparedPublicRoot> prepared;
            world_interactions::ExecuteResult ready{};
            {
                RemoteApplyScope scope;
                auto event = pending.root;
                event.relativeDelaySeconds = (std::max)(0.0, event.relativeDelaySeconds -
                    static_cast<double>(TickMs() - pending.receivedAt) / 1000.0);
                world_actions::SetRemoteCauseEnvelope(
                    pending.mutation.sourcePeer, pending.mutation.sequence);
                ready = world_actions::Prepare(
                    event, pending.mutation.entity, *gRegistry, prepared);
            }
            if (!prepared) {
                WorldMutationResultCode code = WorldMutationResultCode::EntityRejected;
                if (ready.status == world_interactions::ExecuteStatus::RuntimeUnavailable)
                    code = WorldMutationResultCode::RuntimePending;
                else if (ready.status == world_interactions::ExecuteStatus::NativeFault)
                    code = WorldMutationResultCode::NativeFault;

                if (!crossedRetail &&
                    ready.status == world_interactions::ExecuteStatus::RuntimeUnavailable &&
                    TickMs() < segment.front().expiresAt) {
                    return requeueWholeBeforeRetail();
                }
                if (!crossedRetail)
                    return rejectWholeBeforeRetail(code);
                return skipFromMember(i, code);
            }

            crossedRetail = true;
            const auto result = ExecuteMutationOnEngineThread(
                pending.mutation, pending.receivedAt, prepared);
            gDeferredExecutedCount.fetch_add(1U, std::memory_order_relaxed);
            if (result.code != WorldMutationResultCode::Accepted || !result.dispatched)
                return skipAfterExecutedMember(i);
        }

        {
            std::lock_guard lock(gOrderingMutex);
            gLastCommittedSequence = segment.back().mutation.sequence;
            for (const auto& member : segment)
                RememberPublicCauseLocked({member.mutation.sourcePeer, member.root.observationId});
            gAcceptedMutationCount.fetch_add(segment.size(), std::memory_order_acq_rel);
        }
        gOrderedCarrierCommitCount.fetch_add(1U, std::memory_order_relaxed);
        gAppliedSegments.fetch_add(1U, std::memory_order_relaxed);
        TouchVersion();
        return segment.size();
    }

    // Patch 4: reconcile semantic state BEFORE dependency/context preflight and
    // BEFORE WorldPublicActions::Prepare. This is the safety boundary proven by
    // the Patch 2 diagnostics: a redundant LogicSwitch ScheduledInput can fault
    // while its NativeContextChain is being reconstructed, so "already desired"
    // must never reach Prepare merely to discover that no retail replay is safe.
    std::vector<semantic_reconciliation::ReconciliationResult> semantic;
    try { semantic.resize(segment.size()); }
    catch (...) {
        for (const auto& member : segment)
            PushResult(Rejected(member.mutation, WorldMutationResultCode::QueueRejected));
        return segment.size();
    }

    bool semanticRetry = false;
    bool semanticReject = false;
    auto semanticRejectCode = WorldMutationResultCode::EntityRejected;
    for (std::size_t i = 0U; i < segment.size(); ++i) {
        const auto& pending = segment[i];
        if (pending.suppressLocalEcho) continue;
        semantic[i] = semantic_reconciliation::EvaluateBeforePrepare(
            *gRegistry, pending.mutation.entity, pending.root, pending.mutation.sequence);
        switch (semantic[i].action) {
        case semantic_reconciliation::ReconciliationAction::RetryRuntimeUnavailable:
            semanticRetry = true;
            break;
        case semantic_reconciliation::ReconciliationAction::RejectNativeFault:
            semanticReject = true;
            semanticRejectCode = WorldMutationResultCode::NativeFault;
            break;
        case semantic_reconciliation::ReconciliationAction::RejectMismatch:
        case semantic_reconciliation::ReconciliationAction::RejectTransitionUnproven:
        case semantic_reconciliation::ReconciliationAction::RejectReaderUnsupported:
        case semantic_reconciliation::ReconciliationAction::RejectInvalidState:
        case semantic_reconciliation::ReconciliationAction::RejectStatefulMultiRoot:
            semanticReject = true;
            break;
        case semantic_reconciliation::ReconciliationAction::LegacyApply:
        case semantic_reconciliation::ReconciliationAction::ApplyRequired:
        case semantic_reconciliation::ReconciliationAction::ApplyCarrierTransition:
        case semantic_reconciliation::ReconciliationAction::SkipAlreadySatisfied:
        default:
            break;
        }
    }

    // A proved semantic mismatch is final for this transaction. No dependency
    // resolution, NativeContext reconstruction, Prepare or retail call occurs.
    // The failed results remain visible, and SettlementFence consumes the
    // skipped sequence range without applying or replaying an unproven effect.
    if (semanticReject) {
        for (const auto& member : segment) {
            CountRejection(semanticRejectCode);
            PushResult(Rejected(member.mutation, semanticRejectCode));
        }
        gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
        return segment.size();
    }

    // RuntimeUnavailable is transient. Requeue the WHOLE transaction without
    // emitting a failing result until the existing deferred lifetime expires.
    // This mirrors the current runtime dependency policy and preserves ordering.
    if (semanticRetry) {
        if (now >= segment.front().expiresAt) {
            gDeferredExpiredCount.fetch_add(segment.size(), std::memory_order_relaxed);
            for (const auto& member : segment) {
                CountRejection(WorldMutationResultCode::RuntimePending);
                PushResult(Rejected(member.mutation, WorldMutationResultCode::RuntimePending));
            }
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            return segment.size();
        }
        gDeferredRetryCount.fetch_add(1U, std::memory_order_relaxed);
        segment.front().nextRetryAt = now + kDeferredRemoteApplyRetryMs;
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        gInFlightRemoteRoots -= inFlight.count; inFlight.count = 0U;
        for (auto it = segment.rbegin(); it != segment.rend(); ++it)
            gDeferredRemoteApplies.push_front(std::move(*it));
        return 0U;
    }

    // Resolve every dependency before the first retail invocation, but only for
    // members that semantic reconciliation says still require retail. A State
    // already satisfied locally bypasses even NativeContext dependency probing.
    for (std::size_t i = 0U; i < segment.size(); ++i) {
        auto& pending = segment[i];
        if (pending.suppressLocalEcho ||
            semantic[i].action == semantic_reconciliation::ReconciliationAction::SkipAlreadySatisfied)
            continue;
        const auto reason = PublicRootPendingReason(pending.mutation, pending.root);
        if (reason == 0U) continue;
        if (now >= segment.front().expiresAt) {
            gDeferredExpiredCount.fetch_add(segment.size(), std::memory_order_relaxed);
            for (const auto& member : segment)
                PushResult(Rejected(member.mutation, WorldMutationResultCode::RuntimePending));
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            return segment.size();
        }
        if (!pending.pendingRecorded) {
            pending.pendingRecorded = true;
            world_actions::RecordDependencyPending(pending.root, pending.mutation.entity,
                pending.mutation.sourcePeer, pending.mutation.sequence, reason);
        }
        gDeferredRetryCount.fetch_add(1U, std::memory_order_relaxed);
        segment.front().nextRetryAt = now + kDeferredRemoteApplyRetryMs;
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        gInFlightRemoteRoots -= inFlight.count; inFlight.count = 0U;
        for (auto it = segment.rbegin(); it != segment.rend(); ++it)
            gDeferredRemoteApplies.push_front(std::move(*it));
        return 0U;
    }

    // Preparation pins references and reconstructs contexts only for members
    // that still require retail. Semantic State skips have no PreparedPublicRoot
    // because they are converged logical successes, not hidden retail calls.
    std::vector<std::shared_ptr<world_actions::PreparedPublicRoot>> prepared;
    try { prepared.resize(segment.size()); }
    catch (...) {
        for (const auto& member : segment)
            PushResult(Rejected(member.mutation, WorldMutationResultCode::QueueRejected));
        return segment.size();
    }
    {
        RemoteApplyScope scope;
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const auto& pending = segment[i];
            if (pending.suppressLocalEcho ||
                semantic[i].action == semantic_reconciliation::ReconciliationAction::SkipAlreadySatisfied)
                continue;
            auto event = pending.root;
            event.relativeDelaySeconds = (std::max)(0.0, event.relativeDelaySeconds -
                static_cast<double>(now - pending.receivedAt) / 1000.0);
            world_actions::SetRemoteCauseEnvelope(pending.mutation.sourcePeer, pending.mutation.sequence);
            const auto ready = world_actions::Prepare(event, pending.mutation.entity, *gRegistry, prepared[i]);
            if (prepared[i]) continue;
            prepared.clear(); // release only on this engine thread
            if (ready.status == world_interactions::ExecuteStatus::RuntimeUnavailable && now < segment.front().expiresAt) {
                segment.front().nextRetryAt = now + kDeferredRemoteApplyRetryMs;
                std::lock_guard lock(gDeferredRemoteApplyMutex);
                gInFlightRemoteRoots -= inFlight.count; inFlight.count = 0U;
                for (auto it = segment.rbegin(); it != segment.rend(); ++it)
                    gDeferredRemoteApplies.push_front(std::move(*it));
                return 0U;
            }
            const auto code = ready.status == world_interactions::ExecuteStatus::NativeFault
                ? WorldMutationResultCode::NativeFault : WorldMutationResultCode::EntityRejected;
            for (const auto& member : segment) PushResult(Rejected(member.mutation, code));
            gFailedSegments.fetch_add(1U, std::memory_order_relaxed);
            return segment.size();
        }
    }
    // No retry once any member crosses retail. Preserve effects already run,
    // discard the unsuccessful suffix, and consume this transaction once.
    bool failed = false;
    for (std::size_t i = 0; i < segment.size(); ++i) {
        const auto& pending = segment[i];
        if (failed) {
            PushResult(Rejected(pending.mutation, WorldMutationResultCode::QueueRejected));
            continue;
        }
        WorldMutationResult result{};
        if (pending.suppressLocalEcho) {
            // Non-Await ROOTs already executed locally by contract. Suppress
            // their authoritative reflection at the transaction execution
            // boundary, not at receipt, so no durable checkpoint advances early.
            static_cast<void>(world_interactions::ConsumeLocalObservation(pending.root.observationId));
            result = {pending.mutation.sequence, WorldMutationResultCode::Accepted, true, true, pending.mutation.nativeDispatchId};
            gEchoSuppressedCount.fetch_add(1U, std::memory_order_acq_rel);
            PushResult(result);
        } else if (semantic[i].action ==
            semantic_reconciliation::ReconciliationAction::SkipAlreadySatisfied) {
            // Active State reconciliation: the semantic target already equals
            // Desired, so this member is logically accepted without touching the
            // apply-boundary object or rebuilding/executing its CATHODE context.
            result = {pending.mutation.sequence, WorldMutationResultCode::Accepted,
                true, true, pending.mutation.nativeDispatchId};
            PushResult(result);
        } else {
            result = ExecuteMutationOnEngineThread(pending.mutation, pending.receivedAt, prepared[i]);
            gDeferredExecutedCount.fetch_add(1U, std::memory_order_relaxed);
        }
        failed = result.code != WorldMutationResultCode::Accepted || !result.dispatched;
    }
    if (!failed) {
        std::lock_guard lock(gOrderingMutex);
        gLastCommittedSequence = segment.back().mutation.sequence;
        for (const auto& member : segment)
            RememberPublicCauseLocked({member.mutation.sourcePeer, member.root.observationId});
        gAcceptedMutationCount.fetch_add(segment.size(), std::memory_order_acq_rel);
    }
    (failed ? gFailedSegments : gAppliedSegments).fetch_add(1U, std::memory_order_relaxed);
    TouchVersion();
    return segment.size();
}

std::size_t PumpDeferredRemoteApplyOnEngineThread(const std::size_t budget) noexcept {
    // Execute complete FIFO transactions. A blocked head stops the batch;
    // no later transaction may overtake it. Bound gameplay-thread time too.
    return DrainOrderedTransactionBatch(budget,
        [](const std::size_t remaining) { return PumpOneDeferredRemoteTransaction(remaining); },
        [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(); });
}

bool TryPopMutationResult(WorldMutationResult& output) noexcept {
    return gResults.Pop(output);
}

bool RemoteApplyActive() noexcept {
    return gRemoteApplyDepth != 0U;
}

void ResetForGeneration(const std::uint64_t generation) noexcept {
    gOutboundIntents.Clear();
    gResults.Clear();
    {
        std::lock_guard lock(gOutboundSubmitMutex);
        gStagedRoots.clear(); // ring Clear invalidated every reservation ticket
    }
    {
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        gDeferredRemoteApplies.clear();
        gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
    }
    {
        std::lock_guard lock(gControlledRemoteApplyMutex);
        gControlledRemoteApplyArmed = false;
        gControlledRemoteApplyTarget = {};
        gControlledRemoteApplyArmSequence = 0U;
        gControlledRemoteApplyBurstUntil = 0U;
        gControlledRemoteApplyRootCount = 0U;
        gControlledRemoteApplyLastSelectedInput = 0U;
        gControlledRemoteApplyLastSelectedTarget = {};
        gControlledRemoteApplyLastSelectedSequence = 0U;
    }
    {
        std::lock_guard lock(gOrderingMutex);
        gLastCommittedSequence = 0U;
        gCausalRecoveryRequired = false;
        gDivergedAuthorityTransaction = 0U;
        gStateRevisions.clear();
        gPublicCauses.clear();
        gPublicCauseOrder.clear();
    }
    gLevelGenerationAtInitialize.store(generation, std::memory_order_release);
    gLocalIntentSequence.store(1U, std::memory_order_release);
    world_interactions::ResetForGeneration();
    world_animated::ResetForGeneration();
    semantic_reconciliation::ResetForGeneration();
    gResetCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

void ResetAuthorityOrdering() noexcept {
    gOutboundIntents.Clear();
    gResults.Clear();
    {
        std::lock_guard lock(gOutboundSubmitMutex);
        gStagedRoots.clear();
    }
    {
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        gDeferredRemoteApplies.clear();
        gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
    }
    {
        std::lock_guard lock(gOrderingMutex);
        gLastCommittedSequence = 0U;
        gCausalRecoveryRequired = false;
        gDivergedAuthorityTransaction = 0U;
        gStateRevisions.clear();
        gPublicCauses.clear();
        gPublicCauseOrder.clear();
    }
    world_interactions::ResetSessionObservations();
    TouchVersion();
}

void BeginLogicalShutdown() noexcept {
    if (!gInitialized.exchange(false, std::memory_order_acq_rel)) return;
    gOutboundIntents.Clear();
    gResults.Clear();
    {
        std::lock_guard lock(gOutboundSubmitMutex);
        gStagedRoots.clear();
    }
    {
        std::lock_guard lock(gDeferredRemoteApplyMutex);
        gDeferredRemoteApplies.clear();
        gReservedRemoteRoots = 0U; gIngressRoot = {}; gIngressPeer = 0U; gIngressNextOrdinal = 0U;
    }
    {
        std::lock_guard lock(gControlledRemoteApplyMutex);
        gControlledRemoteApplyArmed = false;
        gControlledRemoteApplyTarget = {};
        gControlledRemoteApplyArmSequence = 0U;
        gControlledRemoteApplyBurstUntil = 0U;
        gControlledRemoteApplyRootCount = 0U;
        gControlledRemoteApplyLastSelectedInput = 0U;
        gControlledRemoteApplyLastSelectedTarget = {};
        gControlledRemoteApplyLastSelectedSequence = 0U;
    }
    {
        std::lock_guard lock(gOrderingMutex);
        gStateRevisions.clear();
        gPublicCauses.clear();
        gPublicCauseOrder.clear();
        gLastCommittedSequence = 0U;
        gCausalRecoveryRequired = false;
        gDivergedAuthorityTransaction = 0U;
    }
    semantic_reconciliation::Shutdown();
    gGenerations = nullptr;
    gDispatcher = nullptr;
    gRegistry = nullptr;
    gShutdownCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

} // namespace aimp::replication_core
