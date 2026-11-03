#include "aimp/WorldActionLineage.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/LocalObservationLedger.h"
#include "aimp/OrderedTransactionBatch.h"
#include "aimp/DeferredDiagnosticBuffer.h"
#include "aimp/PlayerLocalControls.h"
#include "aimp/WorldSemanticReconciliation.h"
#include "aimp/CausalPublicationPolicy.h"
#include "aimp/IpcWriteBatch.h"
#include "aimp/SelfClockedRootFilter.h"

#if defined(NDEBUG)
#undef NDEBUG // These checks must also execute in the Release test binary.
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>

int main() {
    using namespace aimp;
    using namespace world_actions;
    using namespace world_interactions;

    // A per-frame CATHODE loop (light flicker) becomes local after its burst;
    // a door toggled by a player keeps replicating. A quiet loop is released.
    {
        SelfClockedRootFilter filter;
        using D = SelfClockedRootFilter::Decision;
        for (std::uint32_t i = 0U; i < SelfClockedRootFilter::kBurstLimit; ++i)
            assert(filter.Observe(42U, 1000U + i * 16U) == D::Replicate);
        assert(filter.Observe(42U, 1200U) == D::BecameLocal);
        assert(filter.Observe(42U, 1216U) == D::Local);
        assert(filter.Observe(42U, 9000U) == D::Replicate);
        for (std::uint64_t t = 0U; t < 20U; ++t)
            assert(filter.Observe(7U, 1000U + t * 400U) == D::Replicate);
        assert(filter.Observe(0U, 1000U) == D::Replicate);
        filter.Clear();
        assert(filter.Observe(42U, 1216U) == D::Replicate);
    }

    // A byte-stream write keeps frame order and honors both batch limits.
    std::deque<std::vector<std::byte>> frames{{std::byte{1}, std::byte{2}},
        {std::byte{3}}, {std::byte{4}, std::byte{5}}};
    std::size_t queuedBytes = 5U;
    std::vector<std::byte> wireBatch;
    assert(TakeIpcWriteBatch(frames, queuedBytes, wireBatch, 64U, 3U) == 2U);
    assert(wireBatch == (std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}));
    assert(queuedBytes == 2U && frames.size() == 1U);
    assert(TakeIpcWriteBatch(frames, queuedBytes, wireBatch, 1U, 1U) == 0U);
    assert(queuedBytes == 2U && frames.size() == 1U);
    assert(TakeIpcWriteBatch(frames, queuedBytes, wireBatch, 1U, 3U) == 1U);
    assert(queuedBytes == 0U && frames.empty() && wireBatch.front() == std::byte{4});

    WorldCompositePublicInputCausalProfile eventProfile{};
    eventProfile.endpointKind = 2U;
    eventProfile.dependencyFlags = WorldCausalDependencyWorldEffect | WorldCausalDependencyPlayer | WorldCausalDependencyUI;
    eventProfile.replicationContractKind = WorldReplicationContractKind::Event;
    eventProfile.semanticTargetEntity = {1U, 2U};
    eventProfile.semanticOperation = 0x4E85BB2DU;
    eventProfile.replicationRelevance = static_cast<std::uint32_t>(WorldReplicationRelevanceDisposition::ReplicateCarrier) |
        WorldReplicationEvidenceEmittedEventCarrier;
    assert(eventProfile.CertifiedEmittedEventCarrierBoundary() && eventProfile.NetworkReplicableBoundary());
    eventProfile.dependencyFlags |= WorldCausalDependencyUnknown;
    assert(!eventProfile.CertifiedEmittedEventCarrierBoundary() && !eventProfile.NetworkReplicableBoundary());
    eventProfile.dependencyFlags = WorldCausalDependencyWorldEffect;
    eventProfile.endpointKind = 3U;
    assert(!eventProfile.CertifiedEmittedEventCarrierBoundary());

    // Regression: thousands of ROOTs in one scheduler turn must become
    // publishable before that turn unwinds. Full transaction segments retain
    // their original 32-member boundaries; the last short group stays private.
    static_assert(replication_core::kWorldIntentQueueCapacity >= 4096U);
    struct PublicationRoot { std::uint64_t cause; unsigned generation, ordinal; };
    std::vector<PublicationRoot> stagedPublication;
    std::vector<unsigned> publishedOrdinals;
    const auto samePublicationGroup = [](const auto& a, const auto& b) {
        return a.cause == b.cause && a.generation == b.generation;
    };
    for (unsigned i = 0U; i < 5000U; ++i) {
        stagedPublication.push_back({1U, 1U, i});
        const auto prefix = replication_core::StableCausalPublicationPrefix(
            stagedPublication, kMaximumTransactionRoots, samePublicationGroup);
        if (prefix != 0U) {
            assert(prefix == kMaximumTransactionRoots);
            for (std::size_t j = 0U; j < prefix; ++j) publishedOrdinals.push_back(stagedPublication[j].ordinal);
            stagedPublication.erase(stagedPublication.begin(), stagedPublication.begin() + prefix);
        }
        assert(stagedPublication.size() < kMaximumTransactionRoots);
    }
    assert(publishedOrdinals.size() == 4992U && stagedPublication.size() == 8U);
    stagedPublication.push_back({1U, 2U, 5000U}); // generation closes the old short group
    const auto shortPrefix = replication_core::StableCausalPublicationPrefix(
        stagedPublication, kMaximumTransactionRoots, samePublicationGroup);
    assert(shortPrefix == 8U);
    for (const auto& pending : stagedPublication) publishedOrdinals.push_back(pending.ordinal); // final seal
    assert(publishedOrdinals.size() == 5001U);
    for (unsigned i = 0U; i < publishedOrdinals.size(); ++i) assert(publishedOrdinals[i] == i);
    stagedPublication = {{1U,1U,0U},{2U,1U,1U},{2U,1U,2U}};
    assert(replication_core::StableCausalPublicationPrefix(stagedPublication,
        kMaximumTransactionRoots, samePublicationGroup) == 1U);

    // A replayed interaction must not consume the receiver's pad or show its
    // action prompt. The initiating player's local controls remain untouched.
    for (const auto type : {0x1728F297U, 0x324DBAB5U, 0x73E92EC1U, 0x71E671A8U}) {
        assert(SuppressRemotePlayerControl(type, true));
        assert(!SuppressRemotePlayerControl(type, false));
    }
    for (const auto worldOrDisplay : {0xAF6549BBU, 0x0E423FCCU, 0xB9B4A894U})
        assert(!SuppressRemotePlayerControl(worldOrDisplay, true));

    // A high-level Start/Stop may have a lazy, absent semantic descendant.
    // Replay the certified parent through retail; never synthesize a child
    // state, skip the event, or relax the lower ScheduledInput reader rules.
    using namespace semantic_reconciliation;
    WorldCompositePublicInputCausalProfile lazyChild{};
    lazyChild.endpointKind = 1U;
    lazyChild.dependencyFlags = WorldCausalDependencyWorldEffect;
    lazyChild.replicationRelevance = static_cast<std::uint32_t>(WorldReplicationRelevanceDisposition::ReplicateCarrier) |
        WorldReplicationEvidenceCompositeStateTransitionCarrier | WorldReplicationEvidenceCompleteDescendantFrontier;
    lazyChild.replicationContractKind = WorldReplicationContractKind::StateTransition;
    lazyChild.semanticTargetEntity = {1U, 1U};
    lazyChild.semanticOperation = 0xD2B3CC2BU;
    assert(CanReplayCarrierWithoutDescendantState(lazyChild, StateReadStatus::RuntimeUnavailable));
    for (const auto fault : {StateReadStatus::NativeFault, StateReadStatus::InvalidState,
         StateReadStatus::ReaderUnsupported, StateReadStatus::Resolved})
        assert(!CanReplayCarrierWithoutDescendantState(lazyChild, fault));
    lazyChild.endpointKind = 3U;
    assert(!CanReplayCarrierWithoutDescendantState(lazyChild, StateReadStatus::RuntimeUnavailable));
    lazyChild.endpointKind = 1U;
    lazyChild.dependencyFlags |= WorldCausalDependencyTruncated;
    assert(!CanReplayCarrierWithoutDescendantState(lazyChild, StateReadStatus::RuntimeUnavailable));

    DeferredDiagnosticBuffer<std::uint64_t, 4U> diagnosticBuffer;
    for (std::uint64_t i = 0; i < 4U; ++i) assert(diagnosticBuffer.Push(i));
    assert(!diagnosticBuffer.Push(99U) && diagnosticBuffer.TakeDropped() == 1U);
    assert(diagnosticBuffer.TakeDropped() == 0U);
    const auto diagnosticBatch = diagnosticBuffer.Drain();
    assert((diagnosticBatch == std::vector<std::uint64_t>{0U, 1U, 2U, 3U}));
    assert(diagnosticBuffer.Push(4U));
    assert((diagnosticBuffer.Drain() == std::vector<std::uint64_t>{4U}));
    assert(diagnosticBuffer.Drain().empty());

    // Burst processing preserves FIFO head blocking, atomic transactions and
    // both work/time limits. No sleeps or engine objects are needed.
    std::size_t calls = 0U;
    const auto fixedClock = [] { return 0ULL; };
    assert(replication_core::DrainOrderedTransactionBatch(64U,
        [&](std::size_t) { ++calls; return 1U; }, fixedClock) == 64U && calls == 64U);
    calls = 0U;
    assert(replication_core::DrainOrderedTransactionBatch(64U,
        [&](std::size_t) { ++calls; return 0U; }, fixedClock) == 0U && calls == 1U);
    calls = 0U;
    assert(replication_core::DrainOrderedTransactionBatch(4U,
        [&](std::size_t) { ++calls; return 32U; }, fixedClock) == 32U && calls == 1U);
    calls = 0U;
    std::uint64_t batchTime = 0U;
    assert(replication_core::DrainOrderedTransactionBatch(64U,
        [&](std::size_t) { ++calls; batchTime += 2U; return 1U; },
        [&] { return batchTime; }) == 1U && calls == 1U);
    calls = 0U;
    assert(replication_core::DrainOrderedTransactionBatch(0U,
        [&](std::size_t) { ++calls; return 1U; }, fixedClock) == 0U && calls == 0U);

    // A delayed authority echo must not exhaust one slot per observation.
    LocalObservationLedger<64U> observations;
    for (std::uint64_t i = 1U; i <= 200000U; ++i) assert(observations.Remember(i));
    assert(observations.Pending() == 200000U && observations.Ranges() == 1U);
    assert(observations.Remember(1500U) && observations.Pending() == 200000U);
    for (std::uint64_t i = 1U; i <= 200000U; ++i) {
        assert(observations.Contains(i));
        assert(observations.Consume(i));
        assert(!observations.Contains(i));
    }
    assert(observations.Pending() == 0U && observations.Ranges() == 0U);
    assert(observations.HighWater() == 200000U);
    assert(observations.Remember(3U) && observations.Remember(1U) && observations.Remember(2U));
    assert(observations.Ranges() == 1U && observations.Consume(2U));
    assert(observations.Ranges() == 2U && observations.Contains(1U) && observations.Contains(3U));
    assert(!observations.Contains(2U) && !observations.Remember(0U));
    observations.Clear();
    assert(observations.Pending() == 0U && observations.HighWater() == 0U);
    LocalObservationLedger<2U> fragmented;
    for (auto serial : {1ULL, 2ULL, 3ULL, 5ULL}) assert(fragmented.Remember(serial));
    assert(!fragmented.Remember(7U) && !fragmented.Consume(2U));
    assert(fragmented.Pending() == 4U && fragmented.Contains(2U) && fragmented.Contains(5U));
    assert(fragmented.SplitRefused() == 1U && fragmented.Consume(5U) && fragmented.Consume(2U));
    assert(fragmented.Contains(1U) && fragmented.Contains(3U) && !fragmented.Contains(2U));
    for (auto salt : {0ULL, 1ULL, 0xDEADBEEF12345678ULL, UINT64_MAX}) {
        for (std::uint64_t serial = 1U; serial < 10000U; ++serial) {
            const auto id = salt ^ (serial * kObservationMultiplier);
            if (id != 0U) assert(ObservationSerial(id, salt) == serial);
        }
    }
    LocalObservationLedger<64U> reordered;
    std::unordered_map<std::uint64_t, bool> expectedObservations;
    std::mt19937 observationRandom(360U);
    for (std::size_t step = 0U; step < 20000U; ++step) {
        const auto serial = 1U + observationRandom() % 512U;
        if (observationRandom() % 2U == 0U) {
            if (reordered.Remember(serial)) expectedObservations[serial] = true;
        } else if (reordered.Consume(serial)) {
            assert(expectedObservations.erase(serial) == 1U);
        }
        assert(reordered.Pending() == expectedObservations.size());
        for (std::uint64_t key = 1U; key <= 512U; ++key)
            assert(reordered.Contains(key) == expectedObservations.contains(key));
    }
    LocalObservationLedger<2U> wrapBoundary;
    assert(wrapBoundary.Remember(UINT64_MAX) && wrapBoundary.Remember(UINT64_MAX - 1U));
    assert(wrapBoundary.Ranges() == 1U && wrapBoundary.Consume(UINT64_MAX));
    assert(wrapBoundary.Contains(UINT64_MAX - 1U) && !wrapBoundary.Contains(UINT64_MAX));

    static_assert(kAllAdmissionSecurityMask == 0x3FFFU);
    static_assert(static_cast<std::uint32_t>(AdmissionSecurityGate::ForbiddenBehaviorInputs) == 0x0001U);
    static_assert(static_cast<std::uint32_t>(AdmissionSecurityGate::ManifestOwnerInput) == 0x0800U);
    static_assert(static_cast<std::uint32_t>(AdmissionSecurityGate::ManifestTargetContract) == 0x1000U);
    static_assert(static_cast<std::uint32_t>(AdmissionSecurityGate::LocalRootDescendants) == 0x2000U);

    LineageTable<64U, 64U> tags;
    assert(tags.Find(0U) == Origin::None);
    assert(!tags.Put(0U, Origin::LocalRoot));
    assert(!tags.Put(1U, Origin::None));
    assert(tags.Put(0x1000U, Origin::LocalRoot));
    assert(tags.Put(0x2000U, tags.Find(0x1000U))); // retained descendant
    tags.Erase(0x1000U); // parent destruction must not orphan the child tag
    assert(tags.Find(0x2000U) == Origin::LocalRoot);
    assert(tags.Put(0x1000U, Origin::RemoteRoot)); // address reuse
    assert(tags.Find(0x1000U) == Origin::RemoteRoot);
    assert(tags.Find(0x2000U) == Origin::LocalRoot);
    assert(tags.Put(0x3000U, Origin::Suppressed));
    assert(tags.Find(0x3000U) == Origin::Suppressed);
    tags.Clear(); // level generation invalidation
    assert(tags.Size() == 0U && tags.Find(0x2000U) == Origin::None);
    for (std::uintptr_t i = 1U; i <= 64U; ++i) assert(tags.Put(i, Origin::LocalRoot));
    assert(!tags.Put(65U, Origin::RemoteRoot));
    for (std::uintptr_t i = 1U; i <= 64U; ++i) assert(tags.Find(i) == Origin::LocalRoot);
    tags.Erase(17U);
    assert(tags.Put(23U, Origin::RemoteRoot)); // deletion holes do not create duplicate keys
    assert(tags.Size() == 63U);
    assert(tags.Put(65U, Origin::RemoteRoot));
    tags.Erase(23U);
    assert(tags.Find(23U) == Origin::None && tags.Size() == 63U);

    // Randomized deletion/reuse/collision history against a reference model.
    tags.Clear();
    std::unordered_map<std::uintptr_t, Origin> reference;
    std::mt19937 random(0x575333U);
    for (unsigned i = 0; i < 50000U; ++i) {
        const std::uintptr_t key = 1U + random() % 128U;
        if (random() % 3U == 0U) { tags.Erase(key); reference.erase(key); }
        else {
            const auto value = random() % 2U == 0U ? Origin::LocalRoot : Origin::RemoteRoot;
            const bool expected = reference.contains(key) || reference.size() < 64U;
            assert(tags.Put(key, value) == expected);
            if (expected) reference[key] = value;
        }
        assert(tags.Size() == reference.size());
        for (std::uintptr_t k = 1U; k <= 128U; ++k)
            assert(tags.Find(k) == (reference.contains(k) ? reference.at(k) : Origin::None));
    }

    // Pre-ROOT causal provenance is independent from LocalRoot/RemoteRoot. A
    // local cause may survive internal World work without suppressing capture.
    CausalProvenanceTable<8U, 8U> provenance;
    const CausalStamp player{CausalSource::Player, 1U};
    const CausalStamp ai{CausalSource::AI, 2U};
    const CausalStamp ui{CausalSource::UI, 3U};
    const CausalStamp aimp{CausalSource::AIMP, 4U};
    assert(!provenance.Find(0U).Valid());
    assert(!provenance.Put(0U, player));
    assert(!provenance.Put(1U, {}));
    assert(provenance.Put(0x1000U, player));
    assert(provenance.Put(0x2000U, provenance.Find(0x1000U)));
    provenance.Erase(0x1000U);
    assert(provenance.Find(0x2000U).SameChain(player));
    assert(provenance.Put(0x2000U, ai));
    assert(provenance.Find(0x2000U).SameChain(ai));
    provenance.Clear();
    assert(provenance.Size() == 0U && !provenance.Find(0x2000U).Valid());
    for (std::uintptr_t i = 1U; i <= 8U; ++i)
        assert(provenance.Put(i, ui));
    assert(!provenance.Put(9U, aimp));

    // Immediate causal-boundary policy: known local domains may CAUSE a World
    // effect without turning their own Player/AI/UI/AIMP mechanics into roots.
    assert(ClassifyCausalBoundary(WorldDomain::World, false) == CausalBoundaryClass::CleanWorldRoot);
    assert(ClassifyCausalBoundary(WorldDomain::World, true, WorldDomain::World) == CausalBoundaryClass::WorldContinuation);
    for (const auto local : {WorldDomain::Player, WorldDomain::AI, WorldDomain::UI, WorldDomain::AIMP})
        assert(ClassifyCausalBoundary(WorldDomain::World, true, local) == CausalBoundaryClass::LocalToWorldBoundary);
    for (const auto local : {WorldDomain::Player, WorldDomain::AI, WorldDomain::UI, WorldDomain::AIMP})
        assert(ClassifyCausalBoundary(local, false) == CausalBoundaryClass::LocalEffect);
    assert(ClassifyCausalBoundary(WorldDomain::Unknown, false) == CausalBoundaryClass::Unknown);
    assert(ClassifyCausalBoundary(WorldDomain::World, true, WorldDomain::Unknown) == CausalBoundaryClass::Unknown);

    // The actual load-time index: fan-out is one port, distinct targets must
    // be bound to that port, and ambiguity/local effects cannot be bypassed.
    PublicRootIndex ports;
    const WorldEntityId owner{1U, 1U}, interfaceId{2U, 2U}, leaf{3U, 3U}, other{4U, 4U};
    WorldCompositePublicInputDescriptor bridge{};
    bridge.ownerCompositeInstance = owner;
    bridge.interfaceVariableEntity = interfaceId;
    bridge.internalTargetEntity = leaf;
    bridge.publicInputShortGuid = 0x12345678U;
    bridge.internalTargetInputShortGuid = 0x87654321U;
    ports.Add(bridge, true, true, true);
    ports.Add(bridge, true, true, true); // duplicate bridge does not create ambiguity
    assert(ports.Known(owner, bridge.publicInputShortGuid));
    assert(ports.Allows(owner, bridge.publicInputShortGuid, owner, false));
    assert(ports.Allows(owner, bridge.publicInputShortGuid, interfaceId, true));
    assert(ports.Allows(owner, bridge.publicInputShortGuid, leaf, false));
    assert(!ports.Allows(owner, bridge.publicInputShortGuid, other, false));
    assert(!ports.Allows(other, bridge.publicInputShortGuid, leaf, false));
    assert(!ports.Allows(owner, bridge.publicInputShortGuid, owner, true, true));
    PublicRootIndex methods;
    auto method = bridge; method.noTypedData = true;
    methods.Add(method, true, true, true);
    assert(methods.Allows(owner, bridge.publicInputShortGuid, owner, true, true));
    assert(methods.Allows(owner, bridge.publicInputShortGuid, interfaceId, true, true));
    assert(methods.Allows(owner, bridge.publicInputShortGuid, leaf, true, true));
    auto fanout = bridge; fanout.internalTargetEntity = other;
    ports.Add(fanout, true, true, true);
    assert(ports.Allows(owner, bridge.publicInputShortGuid, other, true));
    auto ambiguous = bridge; ambiguous.interfaceVariableEntity = other;
    ports.Add(ambiguous, true, true, true);
    assert(!ports.Known(owner, bridge.publicInputShortGuid));
    ports.Clear(); // generation/manifest invalidation
    assert(!ports.Allows(owner, bridge.publicInputShortGuid, owner, false));
    ports.Add(bridge, true, true, false); // unknown endpoint: no extension
    assert(ports.Allows(owner, bridge.publicInputShortGuid, owner, false));
    assert(!ports.Allows(owner, bridge.publicInputShortGuid, owner, true));
    assert(!ports.Allows(owner, bridge.publicInputShortGuid, leaf, false));
    ports.Clear();
    ports.Add(bridge, true, true, false, true); // known local effect: no ROOT
    assert(!ports.Known(owner, bridge.publicInputShortGuid));
    for (const auto forbidden : {kActivateBehavior, kDeactivateBehavior}) {
        ports.Clear(); auto control = bridge; control.internalTargetInputShortGuid = forbidden;
        ports.Add(control, true, true, true);
        assert(!ports.Known(owner, bridge.publicInputShortGuid));
        control = bridge; control.publicInputShortGuid = forbidden;
        ports.Add(control, true, true, true);
        assert(!ports.Known(owner, forbidden));
    }

    CompositePublicInputEvent event{};
    event.publicInputShortGuid = 0x12345678U;
    event.observationId = 0x0102030405060708ULL;
    event.retailRoot = true;
    event.targetEntity = {0x1122334455667788ULL, 0x8877665544332211ULL};
    event.relativeDelaySeconds = 1.5;
    event.flags = kPublicRootExternalInvocation;
    event.contextKind = PublicContextKind::Autonomous;
    event.causalSerial = 1U;
    event.segmentSerial = 2U;
    WorldPayload payload{};
    assert(EncodeCompositePublicInputPayload(event, payload));
    // The v13 ROOT header retains the original identity/delay prefix and adds
    // bounded context, actor, and transaction fields through byte 95.
    const std::array<std::byte, 40U> goldenPrefix{
        std::byte{13},std::byte{7},std::byte{1},std::byte{1},
        std::byte{0x78},std::byte{0x56},std::byte{0x34},std::byte{0x12},
        std::byte{8},std::byte{7},std::byte{6},std::byte{5},
        std::byte{4},std::byte{3},std::byte{2},std::byte{1},
        std::byte{0x88},std::byte{0x77},std::byte{0x66},std::byte{0x55},
        std::byte{0x44},std::byte{0x33},std::byte{0x22},std::byte{0x11},
        std::byte{0x11},std::byte{0x22},std::byte{0x33},std::byte{0x44},
        std::byte{0x55},std::byte{0x66},std::byte{0x77},std::byte{0x88},
        std::byte{0},std::byte{0},std::byte{0},std::byte{0},
        std::byte{0},std::byte{0},std::byte{0xF8},std::byte{0x3F}};
    assert(payload.size == kCompositePublicRootHeaderBytes);
    assert(std::equal(goldenPrefix.begin(), goldenPrefix.end(), payload.bytes.begin()));
    assert(payload.bytes[40] == std::byte{1}); // autonomous context
    assert(payload.bytes[41] == std::byte{0}); // authority actor
    assert(payload.bytes[82] == std::byte{1}); // one ROOT in the transaction
    auto bytes = std::span<const std::byte>(payload.bytes.data(), payload.size);
    CompositePublicInputEvent decoded{};
    assert(DecodeCompositePublicInputPayload(bytes, decoded));
    assert(decoded.retailRoot && decoded.publicInputShortGuid == event.publicInputShortGuid);
    assert(decoded.observationId == event.observationId && decoded.targetEntity == event.targetEntity);
    assert(decoded.relativeDelaySeconds == 1.5 && decoded.flags == kPublicRootExternalInvocation);
    for (unsigned version = 0U; version < 256U; ++version) {
        payload.bytes[0] = static_cast<std::byte>(version);
        assert(DecodeCompositePublicInputPayload(bytes, decoded) ==
            (version == kCompositePublicRootPayloadVersion ||
             version == kCompositePublicRootPreviousPayloadVersion));
    }
    payload.bytes[0] = static_cast<std::byte>(kCompositePublicRootPayloadVersion);
    for (unsigned i : {1U, 2U, 3U}) {
        const auto saved = payload.bytes[i]; payload.bytes[i] = std::byte{0xFF};
        assert(!DecodeCompositePublicInputPayload(bytes, decoded)); payload.bytes[i] = saved;
    }
    for (std::size_t size = 0; size < kCompositePublicRootHeaderBytes; ++size)
        assert(!DecodeCompositePublicInputPayload(bytes.first(size), decoded));
    assert(!DecodeCompositePublicInputPayload(
        std::span<const std::byte>(payload.bytes.data(), kCompositePublicRootHeaderBytes + 1U), decoded));
    for (const double invalid : {-1.0, 601.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
        event.relativeDelaySeconds = invalid;
        assert(!EncodeCompositePublicInputPayload(event, payload));
        const auto bits = std::bit_cast<std::uint64_t>(invalid);
        for (std::size_t i = 0; i < 8U; ++i)
            payload.bytes[32U + i] = static_cast<std::byte>((bits >> (i * 8U)) & 0xFFU);
        assert(!DecodeCompositePublicInputPayload(bytes, decoded));
    }
    event.relativeDelaySeconds = 600.0;
    assert(EncodeCompositePublicInputPayload(event, payload));
    assert(DecodeCompositePublicInputPayload(bytes, decoded) && decoded.relativeDelaySeconds == 600.0);
    std::fill(payload.bytes.begin() + 16U, payload.bytes.begin() + 32U, std::byte{0});
    assert(!DecodeCompositePublicInputPayload(bytes, decoded));
    event.targetEntity = {};
    assert(!EncodeCompositePublicInputPayload(event, payload));
    event.targetEntity = owner;
    event.flags = kPublicRootDetachedCallback;
    assert(!EncodeCompositePublicInputPayload(event, payload)); // requires a certified native context chain
    event.flags = static_cast<std::uint8_t>(kPublicRootExternalInvocation | kPublicRootDetachedCallback);
    assert(!EncodeCompositePublicInputPayload(event, payload));
    event.flags = 0x80U;
    assert(!EncodeCompositePublicInputPayload(event, payload));
    event.flags = 0U;
    event.observationId = 0U;
    assert(!EncodeCompositePublicInputPayload(event, payload));
    event.observationId = 1U; event.publicInputShortGuid = 0U;
    assert(!EncodeCompositePublicInputPayload(event, payload));
    // WS2 rollback v1 remains decodeable; obsolete ROOT v4/v5 are not upgraded.
    std::copy(goldenPrefix.begin(), goldenPrefix.end(), payload.bytes.begin());
    payload.bytes[0] = std::byte{1}; payload.bytes[2] = std::byte{0};
    payload.bytes[3] = std::byte{0};
    assert(DecodeCompositePublicInputPayload(bytes.first(16U), decoded) && !decoded.retailRoot);
    payload.bytes[0] = std::byte{4};
    assert(!DecodeCompositePublicInputPayload(bytes.first(16U), decoded));
    payload.bytes[0] = std::byte{5};
    assert(!DecodeCompositePublicInputPayload(bytes, decoded));
    std::cout << "PASS: lineage/provenance lifetime/collisions/overflow/suppression; causal-boundary policy; public-port target contracts; ROOT v13 wire, context/flags, delay/schema/identity/version validation\n";
}
