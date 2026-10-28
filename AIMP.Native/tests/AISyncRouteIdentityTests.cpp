#include "aimp/AISyncRouteIdentity.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>

using namespace aimp::ai_sync::route_identity;

namespace {
std::size_t checks = 0;
void Check(bool condition, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "AISync route identity invariant failed at line " << line << '\n';
        std::abort();
    }
}
#define CHECK(value) Check((value), __LINE__)

ResourceKey Resource(std::uint8_t digest = 1, std::uint32_t state = 0) {
    ResourceKey key{};
    constexpr char level[] = "TECH_COMMS";
    std::copy(std::begin(level), std::end(level), key.level.begin());
    key.immutableDigest[0] = digest;
    key.state = state;
    return key;
}
CallerVerifiedBinding Binding(ResourceKey key = Resource()) {
    CallerVerifiedBinding binding{};
    binding.resource = key;
    binding.owner = LoadIncarnation{1, 1, 101, 201};
    binding.layout = HandleLayout{4, 2, 26, 4, 16};
    binding.localSlot = 1;
    binding.localSalt = 3;
    binding.elementCount = 12;
    binding.resourceAndLocalMeshVerified = true;
    binding.storageLifetimeVerified = true;
    return binding;
}
std::unique_ptr<Registry> NewRegistry(std::uint64_t world = 1) {
    // Registry is bounded but deliberately sizable; avoid the Windows default
    // thread stack limit when comparing two independent simulated peers.
    auto registry = std::make_unique<Registry>();
    CHECK(registry->BeginWorld(world) == Result::Ok);
    return registry;
}
BindingIncarnation Bind(Registry& registry, const CallerVerifiedBinding& binding,
    std::span<const LinkKey> links = {}) {
    BindingIncarnation ticket{};
    CHECK(registry.BindCallerVerified(binding, links, ticket) == Result::Ok);
    CHECK(ticket.owner == binding.owner && ticket.serial != 0 && ticket.registry != 0);
    return ticket;
}
LinkKey Link(ResourceKey resource = Resource()) {
    return LinkKey{resource, 0, ElementKey{resource, 2}, ElementKey{resource, 9},
        ContextPair{0x11223344U, 0xAABBCCDDU}};
}
ElementMapping Map(Registry& registry, ResourceKey resource = Resource(), std::uint32_t ordinal = 7) {
    ElementMapping mapping{};
    CHECK(registry.MapElement(ElementKey{resource, ordinal}, mapping) == Result::Ok);
    CHECK(registry.Revalidate(mapping) == Result::Ok);
    return mapping;
}

void DifferentPeerSlots() {
    auto host = NewRegistry();
    auto client = NewRegistry();
    const auto hostBinding = Binding();
    auto clientBinding = Binding();
    clientBinding.owner = LoadIncarnation{1, 4, 900, 1000};
    clientBinding.layout = HandleLayout{5, 3, 24, 8, 32};
    clientBinding.localSlot = 6;
    clientBinding.localSalt = 9;
    const auto hostTicket = Bind(*host, hostBinding);
    const auto clientTicket = Bind(*client, clientBinding);
    const auto hostMapping = Map(*host);
    const auto clientMapping = Map(*client);
    CHECK(hostMapping.localReference == (7U | (1U << 4) | (3U << 6)));
    CHECK(clientMapping.localReference == (7U | (6U << 5) | (9U << 8)));
    CHECK(hostMapping.localReference != clientMapping.localReference);
    CHECK(hostMapping.key == clientMapping.key);
    CHECK(hostMapping.binding == hostTicket && clientMapping.binding == clientTicket);
    ElementKey exported{};
    CHECK(host->ExportElement(hostTicket, hostMapping.localReference, exported) == Result::Ok);
    CHECK(exported == clientMapping.key);
    CHECK(client->ExportElement(clientTicket, hostMapping.localReference, exported) == Result::InvalidReference);
    CHECK(exported == ElementKey{});
    CHECK(client->Revalidate(hostMapping) == Result::StaleBinding);
    auto tampered = clientMapping;
    ++tampered.localReference;
    CHECK(client->Revalidate(tampered) == Result::StaleBinding);
    tampered = clientMapping;
    ++tampered.key.ordinal;
    CHECK(client->Revalidate(tampered) == Result::StaleBinding);
    tampered = clientMapping;
    ++tampered.binding.owner.storageLifetime;
    CHECK(client->Revalidate(tampered) == Result::StaleBinding);
    auto inplace = clientMapping;
    CHECK(client->MapElement(inplace.key, inplace) == Result::Ok);
    CHECK(inplace == clientMapping);
}

void ExactResourceNamespace() {
    auto registry = NewRegistry();
    auto first = Binding(Resource(1, 0));
    auto second = Binding(Resource(2, 0));
    second.owner = LoadIncarnation{1, 2, 102, 202};
    auto third = Binding(Resource(1, 1));
    third.owner = LoadIncarnation{1, 3, 103, 203};
    // All sampled headers were (0,0,0). They coexist only in independently
    // verified local meshes, with exact digest AND level/state namespaces.
    CHECK(first.resource.headerTuple == second.resource.headerTuple);
    CHECK((first.resource.headerTuple == std::array<std::uint32_t, 3>{}));
    static_cast<void>(Bind(*registry, first));
    static_cast<void>(Bind(*registry, second));
    static_cast<void>(Bind(*registry, third));
    const auto a = Map(*registry, first.resource);
    const auto b = Map(*registry, second.resource);
    const auto c = Map(*registry, third.resource);
    CHECK(a.key != b.key && a.key != c.key && b.key != c.key);
    CHECK(a.localReference == b.localReference && a.localReference == c.localReference);
    CHECK(a.binding != b.binding && a.binding != c.binding);
    auto missing = first.resource;
    missing.immutableDigest[31] = 1;
    ElementMapping output = a;
    CHECK(registry->MapElement(ElementKey{missing, 7}, output) == Result::MissingResource);
    CHECK(output == ElementMapping{});
    missing = first.resource;
    ++missing.headerTuple[2];
    CHECK(registry->MapElement(ElementKey{missing, 7}, output) == Result::MissingResource);
    missing = first.resource;
    missing.level[0] = 'S';
    CHECK(registry->MapElement(ElementKey{missing, 7}, output) == Result::MissingResource);
    missing = first.resource;
    ++missing.formatVersion;
    CHECK(registry->MapElement(ElementKey{missing, 7}, output) == Result::InvalidResource);
}

void SaltOneReloadAndWorld() {
    auto registry = NewRegistry();
    auto binding = Binding();
    binding.layout = HandleLayout{4, 0, 28, 1, 12};
    binding.localSlot = 0;
    binding.localSalt = 1;
    const auto original = Bind(*registry, binding);
    const auto oldMapping = Map(*registry);
    CHECK(registry->BeginRetire(original) == Result::Ok);
    CHECK(registry->Revalidate(oldMapping) == Result::StaleBinding);
    ElementKey exported{};
    CHECK(registry->ExportElement(original, oldMapping.localReference, exported) == Result::StaleBinding);
    ElementMapping output = oldMapping;
    CHECK(registry->MapElement(oldMapping.key, output) == Result::StaleBinding);
    CHECK(output == ElementMapping{});
    CHECK(registry->BeginRetire(original) == Result::Ok);
    CHECK(registry->Revoke(original) == Result::Ok);
    CHECK(registry->Revoke(original) == Result::Ok);
    CHECK(registry->BeginRetire(original) == Result::Ok);
    BindingIncarnation rejected = original;
    CHECK(registry->BindCallerVerified(binding, {}, rejected) != Result::Ok);
    CHECK(rejected == BindingIncarnation{});

    binding.owner = LoadIncarnation{1, 2, 102, 202};
    const auto reloaded = Bind(*registry, binding);
    const auto newMapping = Map(*registry);
    CHECK(newMapping.localReference == oldMapping.localReference);
    CHECK(newMapping.binding != oldMapping.binding);
    CHECK(registry->Revalidate(oldMapping) == Result::StaleBinding);
    CHECK(registry->ExportElement(original, newMapping.localReference, exported) == Result::StaleBinding);
    CHECK(registry->ExportElement(reloaded, newMapping.localReference, exported) == Result::Ok);

    CHECK(registry->BeginWorld(0) == Result::InvalidIncarnation);
    CHECK(registry->BeginWorld(1) == Result::InvalidIncarnation);
    CHECK(registry->Revalidate(newMapping) == Result::Ok);
    registry->EndWorld();
    CHECK(registry->Revalidate(newMapping) == Result::WrongWorld);
    CHECK(registry->BeginWorld(1) == Result::InvalidIncarnation);
    CHECK(registry->BeginWorld(2) == Result::Ok);
    CHECK(registry->Revalidate(newMapping) == Result::WrongWorld);
    binding.owner.world = 2;
    binding.owner.load = 1; // Load IDs and native values may repeat in a NEW world.
    const auto nextWorld = Bind(*registry, binding);
    CHECK(nextWorld.serial > reloaded.serial);
    CHECK(Map(*registry).localReference == oldMapping.localReference);
    CHECK(registry->Revalidate(oldMapping) == Result::WrongWorld);
    CHECK(registry->BeginRetire(original) == Result::WrongWorld);
    CHECK(registry->Revoke(original) == Result::WrongWorld);
    CHECK(registry->RevokeStorageLifetime(1, binding.owner.storageLifetime) == Result::WrongWorld);

    registry->EndWorld();
    CHECK(registry->BeginWorld((std::numeric_limits<std::uint64_t>::max)()) == Result::Ok);
    registry->EndWorld();
    CHECK(registry->BeginWorld(1) == Result::InvalidIncarnation);
}

void RegistryRecreation() {
    auto original = NewRegistry();
    auto independent = NewRegistry();
    const auto binding = Binding();
    const auto originalTicket = Bind(*original, binding);
    const auto otherTicket = Bind(*independent, binding);
    const auto oldMapping = Map(*original);
    CHECK(originalTicket.owner == otherTicket.owner);
    CHECK(originalTicket.serial == otherTicket.serial);
    CHECK(originalTicket.registry != otherTicket.registry);
    CHECK(independent->Revalidate(oldMapping) == Result::StaleBinding);
    CHECK(independent->BeginRetire(originalTicket) == Result::StaleBinding);
    CHECK(independent->Revoke(originalTicket) == Result::StaleBinding);
    original = NewRegistry();
    static_cast<void>(Bind(*original, binding));
    const auto current = Map(*original);
    CHECK(current.localReference == oldMapping.localReference);
    CHECK(original->Revalidate(oldMapping) == Result::StaleBinding);
    ElementKey key{};
    CHECK(original->ExportElement(originalTicket, current.localReference, key) == Result::StaleBinding);
}

void LayoutAndBounds() {
    const HandleLayout layout{4, 2, 26, 4, 16};
    CHECK(Validate(layout) == Result::Ok);
    std::uint32_t output = 0xCAFEU;
    auto encode = [&](HandleLayout current, std::uint32_t salt = 3, std::uint32_t slot = 1,
        std::uint32_t count = 12, std::uint32_t ordinal = 7) {
        output = 0xCAFEU;
        return EncodeLocalReference(current, salt, slot, count, ordinal, output);
    };
    CHECK(encode(layout, 0) == Result::InvalidSalt && output == 0xCAFEU);
    CHECK(encode(layout, 1U << 26) == Result::InvalidSalt && output == 0xCAFEU);
    CHECK(encode(layout, 3, 4) == Result::InvalidSlot);
    CHECK(encode(layout, 3, (std::numeric_limits<std::uint32_t>::max)()) == Result::InvalidSlot);
    CHECK(encode(layout, 3, 1, 0) == Result::InvalidOrdinal);
    CHECK(encode(layout, 3, 1, 17) == Result::InvalidOrdinal);
    CHECK(encode(layout, 3, 1, 12, 12) == Result::InvalidOrdinal);
    CHECK(encode(layout, 3, 1, 12, (std::numeric_limits<std::uint32_t>::max)()) == Result::InvalidOrdinal);
    for (std::uint32_t bits : {23U, 31U, 32U, 33U, 255U, (std::numeric_limits<std::uint32_t>::max)()}) {
        auto bad = layout; bad.elementBits = bits;
        CHECK(encode(bad) == Result::InvalidLayout && output == 0xCAFEU);
        bad = layout; bad.slotBits = bits;
        CHECK(encode(bad) == Result::InvalidLayout);
        bad = layout; bad.saltBits = bits;
        CHECK(encode(bad) == Result::InvalidLayout);
    }
    for (const auto bad : {HandleLayout{0, 0, 31, 0, 1}, HandleLayout{0, 0, 31, 1, 0},
        HandleLayout{12, 11, 9, 2048, 4096}, HandleLayout{4, 2, 25, 4, 16},
        HandleLayout{3, 2, 27, 4, 16}, HandleLayout{4, 1, 27, 4, 16}})
        CHECK(encode(bad) == Result::InvalidLayout);
    // Non-power-of-two capacities must reject unused slot/index bit patterns.
    const HandleLayout nonPower{4, 2, 26, 3, 13};
    CHECK(Validate(nonPower) == Result::Ok);
    CHECK(encode(nonPower, (1U << 26) - 1, 2, 13, 12) == Result::Ok);
    CHECK(encode(nonPower, 1, 3, 13, 1) == Result::InvalidSlot);
    CHECK(encode(nonPower, 1, 1, 13, 13) == Result::InvalidOrdinal);
    CHECK(encode(HandleLayout{0, 0, 31, 1, 1}, 0x7FFFFFFFU, 0, 1, 0) == Result::Ok);
    CHECK(output == 0x7FFFFFFFU);
    CHECK(encode(HandleLayout{0, 0, 31, 1, 1}, 0x80000000U, 0, 1, 0) == Result::InvalidSalt);
    CHECK(encode(HandleLayout{22, 0, 10, 1, 1U << 22}, 1023, 0, 1U << 22,
        (1U << 22) - 1) == Result::Ok);
    CHECK(output == 0xFFFFFFFFU);
}

void DirectedSpecialLinks() {
    auto registry = NewRegistry();
    auto binding = Binding();
    binding.specialLinkCount = 1;
    const std::array links{Link()};
    const auto ticket = Bind(*registry, binding, links);
    LinkMapping mapped{};
    CHECK(registry->MapLink(links[0], mapped) == Result::Ok);
    CHECK(mapped.binding == ticket);
    CHECK(mapped.incoming.key.ordinal == 2 && mapped.outgoing.key.ordinal == 9);
    CHECK(mapped.key.context.first == 0x11223344U && mapped.key.context.second == 0xAABBCCDDU);
    CHECK(registry->Revalidate(mapped) == Result::Ok);
    auto inplace = mapped;
    CHECK(registry->MapLink(inplace.key, inplace) == Result::Ok);
    CHECK(inplace == mapped);

    // The same context pair is valid under a different exact resource key.
    auto other = Binding(Resource(2));
    other.owner = LoadIncarnation{1, 2, 102, 202};
    other.specialLinkCount = 1;
    const std::array otherLinks{Link(other.resource)};
    static_cast<void>(Bind(*registry, other, otherLinks));
    LinkMapping otherMapping{};
    CHECK(registry->MapLink(otherLinks[0], otherMapping) == Result::Ok);
    CHECK(otherMapping.key.context == mapped.key.context);
    CHECK(otherMapping.binding != mapped.binding);
    auto reverse = links[0];
    std::swap(reverse.incoming, reverse.outgoing);
    LinkMapping output = mapped;
    CHECK(registry->MapLink(reverse, output) == Result::LinkMismatch);
    CHECK(output == LinkMapping{});
    reverse = links[0];
    std::swap(reverse.context.first, reverse.context.second);
    CHECK(registry->MapLink(reverse, output) == Result::LinkMismatch);
    reverse = links[0];
    ++reverse.ordinal;
    CHECK(registry->MapLink(reverse, output) == Result::InvalidOrdinal);
    auto altered = mapped;
    ++altered.outgoing.binding.serial;
    CHECK(registry->Revalidate(altered) == Result::StaleBinding);
    altered = mapped;
    ++altered.incoming.localReference;
    CHECK(registry->Revalidate(altered) == Result::StaleBinding);
    CHECK(registry->BeginRetire(ticket) == Result::Ok);
    CHECK(registry->Revalidate(mapped) == Result::StaleBinding);
    CHECK(registry->MapLink(links[0], output) == Result::StaleBinding);
}

void CrossResourceEndpoints() {
    auto registry = NewRegistry();
    auto owner = Binding();
    owner.specialLinkCount = 1;
    auto endpoint = Binding(Resource(2));
    endpoint.owner = LoadIncarnation{1, 2, 102, 202};
    auto link = Link();
    link.outgoing.resource = endpoint.resource;
    const std::array links{link};
    const auto ownerTicket = Bind(*registry, owner, links);
    LinkMapping mapping{};
    CHECK(registry->MapLink(link, mapping) == Result::MissingResource);
    const auto endpointTicket = Bind(*registry, endpoint);
    CHECK(registry->MapLink(link, mapping) == Result::Ok);
    CHECK(mapping.binding == ownerTicket && mapping.outgoing.binding == endpointTicket);
    CHECK(registry->Revalidate(mapping) == Result::Ok);
    CHECK(registry->RevokeStorageLifetime(1, endpoint.owner.storageLifetime) == Result::Ok);
    CHECK(registry->Revalidate(mapping) == Result::StaleBinding);
    CHECK(registry->RevokeStorageLifetime(1, endpoint.owner.storageLifetime) == Result::Ok);
    endpoint.owner = LoadIncarnation{1, 3, 103, 203};
    static_cast<void>(Bind(*registry, endpoint));
    CHECK(registry->Revalidate(mapping) == Result::StaleBinding);
    CHECK(registry->MapLink(link, mapping) == Result::Ok);
    CHECK(registry->Revalidate(mapping) == Result::Ok);
}

void DuplicatesAndAmbiguity() {
    auto registry = NewRegistry();
    auto binding = Binding();
    const auto ticket = Bind(*registry, binding);
    const auto mapping = Map(*registry);
    BindingIncarnation rejected{};
    CHECK(registry->BindCallerVerified(binding, {}, rejected) == Result::DuplicateBinding);
    CHECK(rejected == BindingIncarnation{});
    CHECK(registry->Revalidate(mapping) == Result::Ok);
    auto conflicting = binding;
    conflicting.owner = LoadIncarnation{1, 2, 102, 202};
    CHECK(registry->BindCallerVerified(conflicting, {}, rejected) == Result::AmbiguousResource);
    CHECK(registry->Revalidate(mapping) == Result::AmbiguousResource);
    ElementMapping output{};
    CHECK(registry->MapElement(mapping.key, output) == Result::AmbiguousResource);
    CHECK(registry->BindCallerVerified(binding, {}, rejected) == Result::AmbiguousResource);
    CHECK(registry->Revoke(ticket) == Result::Ok);
    static_cast<void>(Bind(*registry, conflicting));
    CHECK(registry->Revalidate(mapping) == Result::StaleBinding);
    CHECK(Map(*registry).binding != mapping.binding);

    registry = NewRegistry();
    const auto slotTicket = Bind(*registry, binding);
    conflicting = binding;
    conflicting.resource = Resource(2);
    ++conflicting.resource.headerTuple[0];
    CHECK(registry->BindCallerVerified(conflicting, {}, rejected) == Result::AmbiguousResource);
    CHECK(registry->BeginRetire(slotTicket) == Result::Ok);

    registry = NewRegistry();
    const auto headerTicket = Bind(*registry, binding);
    conflicting = binding;
    conflicting.resource = Resource(2);
    ++conflicting.localSlot; // Unique slot cannot rescue a duplicate native tuple.
    CHECK(registry->BindCallerVerified(conflicting, {}, rejected) == Result::AmbiguousResource);
    CHECK(registry->Revoke(headerTicket) == Result::Ok);

    registry = NewRegistry();
    binding.specialLinkCount = 1;
    std::array links{Link()};
    const auto linkTicket = Bind(*registry, binding, links);
    std::swap(links[0].incoming, links[0].outgoing);
    CHECK(registry->BindCallerVerified(binding, links, rejected) == Result::AmbiguousResource);
    CHECK(registry->Revoke(linkTicket) == Result::Ok);
}

void CallerProofAndInvalidBindings() {
    auto registry = NewRegistry();
    BindingIncarnation output{};
    auto check = [&](CallerVerifiedBinding bad, Result expected) {
        output.serial = 99;
        CHECK(registry->BindCallerVerified(bad, {}, output) == expected);
        CHECK(output == BindingIncarnation{});
    };
    auto binding = Binding();
    auto bad = binding; bad.resourceAndLocalMeshVerified = false;
    check(bad, Result::UnverifiedBinding);
    bad = binding; bad.storageLifetimeVerified = false;
    check(bad, Result::UnverifiedBinding);
    check(CallerVerifiedBinding{}, Result::UnverifiedBinding);
    bad = binding; bad.resource.level.fill('\0'); check(bad, Result::InvalidResource);
    bad = binding; bad.resource.level.fill('A'); check(bad, Result::InvalidResource);
    bad = binding; bad.resource.level[0] = 't'; check(bad, Result::InvalidResource);
    bad = binding; bad.resource.level[20] = 'X'; check(bad, Result::InvalidResource);
    bad = binding; bad.resource.immutableDigest.fill(0); check(bad, Result::InvalidResource);
    bad = binding; bad.resource.formatVersion = 0; check(bad, Result::InvalidResource);
    bad = binding; bad.owner.world = 0; check(bad, Result::InvalidIncarnation);
    bad = binding; bad.owner.load = 0; check(bad, Result::InvalidIncarnation);
    bad = binding; bad.owner.localMesh = 0; check(bad, Result::InvalidIncarnation);
    bad = binding; bad.owner.storageLifetime = 0; check(bad, Result::InvalidIncarnation);
    bad = binding; bad.owner.world = 2; check(bad, Result::WrongWorld);
    bad = binding; bad.localSalt = 0; check(bad, Result::InvalidSalt);
    bad = binding; bad.localSlot = 4; check(bad, Result::InvalidSlot);
    bad = binding; bad.elementCount = 0; check(bad, Result::InvalidOrdinal);
    bad = binding; bad.elementCount = 17; check(bad, Result::InvalidOrdinal);
    bad = binding; bad.layout.saltBits = 31; check(bad, Result::InvalidLayout);
    bad = binding; bad.specialLinkCount = 1; check(bad, Result::InvalidOrdinal);
    bad = binding; bad.specialLinkCount = static_cast<std::uint32_t>(kMaxLinks + 1);
    check(bad, Result::InvalidOrdinal);
    const auto ticket = Bind(*registry, binding);
    static_cast<void>(Map(*registry)); // Rejected candidates never published partial state.
    CHECK(registry->RevokeStorageLifetime(1, 0) == Result::InvalidIncarnation);
    CHECK(registry->RevokeStorageLifetime(1, 999) == Result::MissingResource);
    auto fake = ticket; ++fake.serial;
    CHECK(registry->BeginRetire(fake) == Result::StaleBinding);
    CHECK(registry->Revoke(fake) == Result::StaleBinding);
    ElementKey key{};
    CHECK(registry->ExportElement(fake, 0, key) == Result::StaleBinding);
    CHECK(registry->ExportElement(ticket, 0, key) == Result::InvalidReference);
    CHECK(registry->ExportElement(ticket, 0xFFFFFFFFU, key) == Result::InvalidReference);
    ElementMapping element{};
    CHECK(registry->MapElement(ElementKey{binding.resource, 12}, element) == Result::InvalidOrdinal);
    CHECK(registry->MapElement(ElementKey{binding.resource, 0xFFFFFFFFU}, element) == Result::InvalidOrdinal);
}

void InvalidLinks() {
    auto registry = NewRegistry();
    auto binding = Binding();
    binding.specialLinkCount = 2;
    std::array links{Link(), Link()};
    BindingIncarnation output{};
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::DuplicateLink);
    links[1].ordinal = 1;
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::AmbiguousContext);
    links[1].context.second = 99;
    links[1].ordinal = 2;
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::InvalidOrdinal);
    links[1].ordinal = 1;
    links[1].incoming.ordinal = binding.elementCount;
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::InvalidOrdinal);
    links[1].incoming.ordinal = 2;
    links[1].outgoing.ordinal = 0xFFFFFFFFU;
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::InvalidOrdinal);
    links[1].outgoing.ordinal = 9;
    links[1].resource = Resource(2);
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::InvalidResource);
    links[1].resource = binding.resource;
    links[1].incoming.resource.immutableDigest.fill(0);
    CHECK(registry->BindCallerVerified(binding, links, output) == Result::InvalidResource);
    links[1].incoming.resource = binding.resource;
    std::swap(links[0], links[1]); // Descriptors need not arrive in ordinal order.
    static_cast<void>(Bind(*registry, binding, links));
    LinkMapping mapping{};
    CHECK(registry->MapLink(links[0], mapping) == Result::Ok);
    CHECK(registry->MapLink(links[1], mapping) == Result::Ok);
}

void SharedStorageAndIncarnations() {
    auto registry = NewRegistry();
    auto first = Binding();
    auto second = first;
    second.resource = Resource(2);
    second.resource.headerTuple[0] = 1;
    second.localSlot = 2;
    const auto a = Bind(*registry, first);
    const auto b = Bind(*registry, second);
    const auto mappedA = Map(*registry, first.resource);
    const auto mappedB = Map(*registry, second.resource);
    CHECK(registry->RevokeStorageLifetime(1, first.owner.storageLifetime) == Result::Ok);
    CHECK(registry->Revalidate(mappedA) == Result::StaleBinding);
    CHECK(registry->Revalidate(mappedB) == Result::StaleBinding);
    CHECK(registry->BeginRetire(a) == Result::Ok && registry->Revoke(b) == Result::Ok);
    BindingIncarnation output{};
    auto reusedSlot = first;
    reusedSlot.resource = Resource(3);
    reusedSlot.resource.headerTuple[0] = 3;
    reusedSlot.owner.storageLifetime = 999;
    CHECK(registry->BindCallerVerified(reusedSlot, {}, output) == Result::StaleBinding);
    auto reload = first;
    ++reload.owner.load;
    ++reload.owner.localMesh;
    // Reusing a revoked storage token fails even for an otherwise new mesh.
    CHECK(registry->BindCallerVerified(reload, {}, output) == Result::InvalidIncarnation);
    ++reload.owner.storageLifetime;
    static_cast<void>(Bind(*registry, reload));
    second.owner.load = 1; // Cannot publish an earlier event after load2.
    CHECK(registry->BindCallerVerified(second, {}, output) == Result::InvalidIncarnation);

    registry = NewRegistry();
    static_cast<void>(Bind(*registry, first));
    second.owner.localMesh = 999;
    CHECK(registry->BindCallerVerified(second, {}, output) == Result::InvalidIncarnation);
    second = first;
    second.resource = Resource(3);
    second.resource.headerTuple[0] = 2;
    second.localSlot = 2;
    second.layout = HandleLayout{5, 2, 25, 4, 32};
    CHECK(registry->BindCallerVerified(second, {}, output) == Result::InvalidIncarnation);
}

void CapacityAndTombstones() {
    auto registry = NewRegistry();
    for (std::size_t i = 0; i < kMaxBindings; ++i) {
        auto binding = Binding(Resource(static_cast<std::uint8_t>(i + 1)));
        binding.owner = LoadIncarnation{1, i + 1, i + 100, i + 200};
        const auto ticket = Bind(*registry, binding);
        CHECK(registry->BeginRetire(ticket) == Result::Ok);
        CHECK(registry->Revoke(ticket) == Result::Ok);
    }
    auto binding = Binding(Resource(200));
    binding.owner = LoadIncarnation{1, kMaxBindings + 1, 9999, 9999};
    BindingIncarnation output{};
    CHECK(registry->BindCallerVerified(binding, {}, output) == Result::CapacityExceeded);
    CHECK(registry->BeginWorld(2) == Result::Ok);
    binding.owner.world = 2;
    static_cast<void>(Bind(*registry, binding));

    registry = NewRegistry();
    auto links = std::make_unique<std::array<LinkKey, kMaxLinks>>();
    binding = Binding();
    binding.specialLinkCount = static_cast<std::uint32_t>(kMaxLinks);
    for (std::size_t i = 0; i < links->size(); ++i) {
        (*links)[i] = Link();
        (*links)[i].ordinal = static_cast<std::uint32_t>(i);
        (*links)[i].context.first = static_cast<std::uint32_t>(i);
    }
    const auto ticket = Bind(*registry, binding, *links);
    LinkMapping mapped{};
    CHECK(registry->MapLink(links->back(), mapped) == Result::Ok);
    CHECK(registry->Revoke(ticket) == Result::Ok);
    auto another = Binding(Resource(2));
    another.owner = LoadIncarnation{1, 2, 102, 202};
    another.specialLinkCount = 1;
    std::array anotherLinks{Link(another.resource)};
    CHECK(registry->BindCallerVerified(another, anotherLinks, output) == Result::CapacityExceeded);
    CHECK(output == BindingIncarnation{});
}

void ExhaustiveSmallHandles() {
    // Independent decoding checks across capacities, slots, salts and ordinals
    // catch width/shift mistakes without mirroring the production encoder.
    for (std::uint32_t capacity = 1; capacity <= 33; ++capacity) {
        std::uint32_t p = 0;
        while ((std::uint64_t{1} << p) < capacity) ++p;
        for (std::uint32_t slots = 1; slots <= 9; ++slots) {
            std::uint32_t t = 0;
            while ((std::uint64_t{1} << t) < slots) ++t;
            const HandleLayout layout{p, t, (std::min)(31U, 32U - p - t), slots, capacity};
            for (std::uint32_t slot = 0; slot < slots; ++slot) {
                for (const std::uint32_t salt : {1U, 3U, 100U}) {
                    for (std::uint32_t ordinal = 0; ordinal < capacity; ++ordinal) {
                        std::uint32_t encoded = 0;
                        CHECK(EncodeLocalReference(layout, salt, slot, capacity, ordinal, encoded) == Result::Ok);
                        const std::uint64_t elementRange = std::uint64_t{1} << p;
                        const std::uint64_t slotRange = std::uint64_t{1} << t;
                        CHECK(encoded % elementRange == ordinal);
                        CHECK((encoded / elementRange) % slotRange == slot);
                        CHECK(encoded / (elementRange * slotRange) == salt);
                    }
                }
            }
        }
    }
}
} // namespace

int main() {
    static_assert(std::is_trivially_copyable_v<ResourceKey>);
    static_assert(std::is_trivially_copyable_v<ElementMapping>);
    static_assert(std::is_trivially_copyable_v<LinkMapping>);
    static_assert(!std::is_copy_constructible_v<Registry>);
    DifferentPeerSlots();
    ExactResourceNamespace();
    SaltOneReloadAndWorld();
    RegistryRecreation();
    LayoutAndBounds();
    DirectedSpecialLinks();
    CrossResourceEndpoints();
    DuplicatesAndAmbiguity();
    CallerProofAndInvalidBindings();
    InvalidLinks();
    SharedStorageAndIncarnations();
    CapacityAndTombstones();
    ExhaustiveSmallHandles();
    std::cout << "AISync route identity: " << checks << " isolated value-layer checks passed\n";
}
