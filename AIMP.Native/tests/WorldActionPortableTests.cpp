#include "aimp/WorldActionLineage.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldPublicActions.h"

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
