#include "aimp/WorldEntityRegistry.h"
#include "aimp/SignatureResolver.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/Diagnostics.h"
#include "aimp/ReplicationCore.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"
#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>

namespace aimp {
struct WorldManifestRuntimeTestAccess {
    static void MissingZone(WorldEntityRegistry& registry, std::uint32_t generation) {
        registry.initialized_ = registry.manifestLoaded_ = true;
        registry.captureReady_.store(true);
        registry.manifestGeneration_ = generation;
        registry.mapContentId_ = {1U, 1U};
        WorldEntityRegistry::RuntimeEntry entry{};
        entry.id = {2U, 2U}; entry.key = {1U, 1U};
        entry.domain = WorldDomain::World; entry.zoneId = 1U;
        registry.entries_.emplace(entry.id, entry);
        registry.loadedZones_[1U] = false;
    }
};
}

int main() {
    using namespace aimp;
    BuildProfile profile{};
    SignatureResolver resolver(nullptr, 0U, profile, false);
    LevelGenerationTracker generations;
    const auto generation = static_cast<std::uint32_t>(generations.BeginTransition().generation);
    Diagnostics diagnostics;
    WorldEntityRegistry registry(resolver, generations, diagnostics);
    WorldManifestRuntimeTestAccess::MissingZone(registry, generation);
    GameThreadDispatcher dispatcher;
    dispatcher.SetGeneration(generation);
    assert(dispatcher.BindEngineThread(GetCurrentThreadId(),
        EngineThreadSource::MainThreadCharacterProcessing, 1U) == EngineThreadBindResult::Bound);
    std::string error;
    assert(replication_core::Initialize(generations, dispatcher, registry, error));
    world_actions::SetSessionAuthority(42U, false);

    const auto mutation = [&](std::uint64_t sequence, std::uint64_t transaction,
                              std::uint16_t ordinal, std::uint16_t count, std::uint64_t peer) {
        world_interactions::CompositePublicInputEvent root{};
        root.retailRoot = true; root.observationId = sequence;
        root.rootKind = world_interactions::PublicRootKind::TypedMessage;
        root.publicInputShortGuid = 65U; root.targetEntity = {2U, 2U};
        root.contextKind = world_interactions::PublicContextKind::Autonomous;
        root.causalSerial = transaction; root.segmentSerial = transaction;
        root.authorityTransaction = transaction; root.rootOrdinal = ordinal; root.rootCount = count;
        WorldMutation value{};
        value.manifest.mapContentId = {1U, 1U}; value.entity = {2U, 2U};
        value.levelGeneration = generation; value.sequence = sequence;
        value.timelineTick = 1U; value.sourcePeer = peer;
        value.flags = WorldMutationFlagReliable;
        value.operation = world_interactions::kOperationCompositePublicInput;
        value.nativeDispatchId = sequence + 100U;
        assert(world_interactions::EncodeCompositePublicInputPayload(root, value.payload));
        return value;
    };
    // A complete two-member transaction waits on an unloaded local zone.
    // The following local-origin echo needs no retail call or zone lookup.
    assert(replication_core::ReceiveAuthoritativeMutation(mutation(1U, 1U, 0U, 2U, 43U)));
    assert(replication_core::ReceiveAuthoritativeMutation(mutation(2U, 1U, 1U, 2U, 43U)));
    assert(replication_core::ReceiveAuthoritativeMutation(mutation(3U, 3U, 0U, 1U, 42U)));
    assert(replication_core::PumpDeferredRemoteApplyOnEngineThread(32U) == 0U);
    WorldMutationResult result{};
    assert(!replication_core::TryPopMutationResult(result));
    std::this_thread::sleep_for(std::chrono::milliseconds(1550));
    for (int i = 0; i < 4; ++i)
        static_cast<void>(replication_core::PumpDeferredRemoteApplyOnEngineThread(32U));
    for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
        assert(replication_core::TryPopMutationResult(result));
        assert(result.sequence == sequence && result.nativeDispatchId == sequence + 100U);
        assert(result.code == (sequence < 3U ? WorldMutationResultCode::RuntimePending : WorldMutationResultCode::Accepted));
        assert(result.dispatched == (sequence == 3U));
    }
    const auto state = replication_core::DiagnosticsSnapshot();
    assert(state.causalLastCommittedSequence == 3U && !state.causalRecoveryRequired);
    assert(state.causalSegmentsSkipped == 1U && state.causalSegmentsApplied == 1U);
    // Retransmission of a skipped transaction is consumed without executing it.
    assert(replication_core::ReceiveAuthoritativeMutation(mutation(1U, 1U, 0U, 2U, 43U)));
    assert(replication_core::TryPopMutationResult(result) && result.dispatched);
    assert(replication_core::DiagnosticsSnapshot().causalSegmentsSkipped == 1U);
    // A new world retains its own validation and starts with a fresh checkpoint.
    replication_core::ResetForGeneration(generations.BeginTransition().generation);
    assert(!replication_core::ReceiveAuthoritativeMutation(mutation(4U, 4U, 0U, 1U, 42U)));
    assert(replication_core::TryPopMutationResult(result) && result.code == WorldMutationResultCode::GenerationRejected);
    assert(replication_core::DiagnosticsSnapshot().causalLastCommittedSequence == 0U);
    replication_core::BeginLogicalShutdown();
    std::cout << "PASS: expired World transaction skipped, next transaction applied, no replay or session closure\n";
}
