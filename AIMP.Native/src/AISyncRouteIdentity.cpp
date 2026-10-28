#include "aimp/AISyncRouteIdentity.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace aimp::ai_sync::route_identity {
namespace {
std::uint64_t NextRegistryId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate != 0) {
        const auto following = candidate == (std::numeric_limits<std::uint64_t>::max)() ? 0 : candidate + 1;
        if (next.compare_exchange_weak(candidate, following, std::memory_order_relaxed)) return candidate;
    }
    return 0; // Permanent exhaustion; never wrap into a previously issued ID.
}
std::uint32_t Width(std::uint32_t capacity) noexcept {
    if (capacity == 0) return 32;
    std::uint32_t bits = 0;
    for (auto remaining = capacity - 1; remaining != 0; remaining >>= 1) ++bits;
    return bits;
}
std::uint64_t Mask(std::uint32_t bits) noexcept {
    return (std::uint64_t{1} << bits) - 1;
}
bool ValidOwner(const LoadIncarnation& owner) noexcept {
    return owner.world != 0 && owner.load != 0 && owner.localMesh != 0 && owner.storageLifetime != 0;
}
bool SameBinding(const CallerVerifiedBinding& a, const CallerVerifiedBinding& b) noexcept {
    return a.resource == b.resource && a.owner == b.owner && a.layout == b.layout &&
        a.localSlot == b.localSlot && a.localSalt == b.localSalt &&
        a.elementCount == b.elementCount && a.specialLinkCount == b.specialLinkCount;
}
} // namespace

Result Validate(const ResourceKey& key) noexcept {
    if (key.formatVersion != kNativeFormatVersion ||
        std::all_of(key.immutableDigest.begin(), key.immutableDigest.end(),
            [](std::uint8_t byte) { return byte == 0; })) return Result::InvalidResource;
    bool terminated = false;
    std::size_t length = 0;
    for (const char value : key.level) {
        if (value == '\0') { terminated = true; continue; }
        if (terminated || !((value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '-')) return Result::InvalidResource;
        ++length;
    }
    return terminated && length != 0 ? Result::Ok : Result::InvalidResource;
}

Result Validate(const HandleLayout& layout) noexcept {
    // Restrict widths BEFORE shifts/additions. Native salt capacity >=10 leaves
    // at most22 combined slot/element bits. The 1x1 case uses31 salt bits.
    if (layout.elementBits > 22 || layout.slotBits > 22 ||
        layout.elementBits + layout.slotBits > 22 ||
        layout.elementBits != Width(layout.elementCapacity) ||
        layout.slotBits != Width(layout.slotCapacity)) return Result::InvalidLayout;
    const auto saltBits = (std::min)(31U, 32U - layout.elementBits - layout.slotBits);
    return layout.saltBits == saltBits && saltBits >= 10 ? Result::Ok : Result::InvalidLayout;
}

Result EncodeLocalReference(const HandleLayout& layout, std::uint32_t salt,
    std::uint32_t slot, std::uint32_t elementCount, std::uint32_t ordinal,
    std::uint32_t& output) noexcept {
    if (Validate(layout) != Result::Ok) return Result::InvalidLayout;
    if (salt == 0 || salt > Mask(layout.saltBits)) return Result::InvalidSalt;
    if (slot >= layout.slotCapacity) return Result::InvalidSlot;
    if (elementCount == 0 || elementCount > layout.elementCapacity || ordinal >= elementCount)
        return Result::InvalidOrdinal;
    const auto reference = std::uint64_t{ordinal} | (std::uint64_t{slot} << layout.elementBits) |
        (std::uint64_t{salt} << (layout.elementBits + layout.slotBits));
    if (reference > (std::numeric_limits<std::uint32_t>::max)()) return Result::InvalidReference;
    output = static_cast<std::uint32_t>(reference);
    return Result::Ok;
}

Registry::Registry() noexcept : registryId_(NextRegistryId()) {}

Result Registry::BeginWorld(std::uint64_t world) noexcept {
    if (registryId_ == 0) return Result::IncarnationExhausted;
    if (world == 0 || world <= lastWorld_) return Result::InvalidIncarnation;
    EndWorld(); // Invalidation precedes reuse of the value slots.
    entryCount_ = 0;
    linkCount_ = 0;
    lastLoad_ = 0;
    world_ = world;
    lastWorld_ = world;
    // Never reset lastSerial_: a stale ticket cannot become current again.
    return Result::Ok;
}
void Registry::EndWorld() noexcept {
    world_ = 0;
    for (std::size_t i = 0; i < entryCount_; ++i) entries_[i].state = State::Revoked;
}

Result Registry::BindCallerVerified(const CallerVerifiedBinding& binding,
    std::span<const LinkKey> links, BindingIncarnation& output) noexcept {
    // No partial ticket is returned on rejection. Caller attestations do not
    // replace the still-required native loader/lifetime implementation.
    output = {};
    if (!binding.resourceAndLocalMeshVerified || !binding.storageLifetimeVerified)
        return Result::UnverifiedBinding;
    if (Validate(binding.resource) != Result::Ok) return Result::InvalidResource;
    if (!ValidOwner(binding.owner)) return Result::InvalidIncarnation;
    if (world_ == 0 || binding.owner.world != world_) return Result::WrongWorld;
    if (binding.owner.load < lastLoad_) return Result::InvalidIncarnation;
    std::uint32_t ignored = 0;
    const auto encoded = EncodeLocalReference(binding.layout, binding.localSalt,
        binding.localSlot, binding.elementCount, 0, ignored);
    if (encoded != Result::Ok) return encoded;
    if (binding.specialLinkCount > kMaxLinks || links.size() != binding.specialLinkCount)
        return Result::InvalidOrdinal;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const auto& link = links[i];
        if (link.resource != binding.resource || Validate(link.incoming.resource) != Result::Ok ||
            Validate(link.outgoing.resource) != Result::Ok) return Result::InvalidResource;
        if (link.ordinal >= binding.specialLinkCount || link.incoming.ordinal >= (1U << 22) ||
            link.outgoing.ordinal >= (1U << 22) ||
            (link.incoming.resource == binding.resource && link.incoming.ordinal >= binding.elementCount) ||
            (link.outgoing.resource == binding.resource && link.outgoing.ordinal >= binding.elementCount))
            return Result::InvalidOrdinal;
        for (std::size_t j = 0; j < i; ++j) {
            if (links[j].ordinal == link.ordinal) return Result::DuplicateLink;
            // Conservative until native lookup is integrated: its context-pair
            // index overwrites duplicates. No cross-resource uniqueness claim.
            if (links[j].context == link.context) return Result::AmbiguousContext;
        }
    }

    bool ambiguous = false;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        auto& entry = entries_[i];
        const auto& previous = entry.value;
        if (entry.state == State::Active && SameBinding(previous, binding)) {
            bool sameLinks = entry.linkCount == links.size();
            for (const auto& link : links) {
                bool found = false;
                for (std::size_t j = 0; j < entry.linkCount; ++j)
                    if (links_[entry.linkBegin + j] == link) found = true;
                sameLinks = sameLinks && found;
            }
            if (sameLinks) return Result::DuplicateBinding;
            entry.state = State::Ambiguous;
            ambiguous = true;
        }
        const bool sameResource = previous.resource == binding.resource;
        const bool sameMesh = previous.owner.localMesh == binding.owner.localMesh;
        const bool conflictingSlot = sameMesh && previous.localSlot == binding.localSlot;
        const bool conflictingHeader = sameMesh && previous.resource.headerTuple == binding.resource.headerTuple;
        if ((entry.state == State::Active || entry.state == State::Ambiguous) &&
            (sameResource || conflictingSlot || conflictingHeader)) {
            entry.state = State::Ambiguous;
            ambiguous = true;
        }
    }
    if (ambiguous) return Result::AmbiguousResource;

    for (std::size_t i = 0; i < entryCount_; ++i) {
        const auto& entry = entries_[i];
        const auto& previous = entry.value;
        const bool sameMesh = previous.owner.localMesh == binding.owner.localMesh;
        const bool sameStorage = previous.owner.storageLifetime == binding.owner.storageLifetime;
        if ((sameMesh && (previous.owner.load != binding.owner.load || previous.layout != binding.layout)) ||
            (previous.owner.load == binding.owner.load && !sameMesh) ||
            (sameStorage && (previous.owner.load != binding.owner.load || entry.state != State::Active)))
            return Result::InvalidIncarnation;
        // Retiring a slot ends that event's authority even if a different
        // resource and a fresh storage token are later presented for the slot.
        // Individual slot recycling is not certified by the Phase3 lifecycle.
        if (sameMesh && entry.state != State::Active &&
            (previous.localSlot == binding.localSlot ||
                previous.resource.headerTuple == binding.resource.headerTuple)) return Result::StaleBinding;
        if (previous.resource == binding.resource && entry.state != State::Active &&
            previous.owner.load == binding.owner.load) return Result::StaleBinding;
    }
    if (entryCount_ == kMaxBindings || links.size() > kMaxLinks - linkCount_)
        return Result::CapacityExceeded;
    if (lastSerial_ == (std::numeric_limits<std::uint64_t>::max)()) return Result::IncarnationExhausted;
    const BindingIncarnation incarnation{binding.owner, lastSerial_ + 1, registryId_};
    auto& entry = entries_[entryCount_];
    entry.value = binding;
    entry.incarnation = incarnation;
    entry.state = State::Active;
    entry.linkBegin = linkCount_;
    entry.linkCount = links.size();
    for (const auto& link : links) links_[linkCount_++] = link;
    ++entryCount_;
    ++lastSerial_;
    lastLoad_ = binding.owner.load;
    output = incarnation;
    return Result::Ok;
}

Result Registry::BeginRetire(const BindingIncarnation& binding) noexcept {
    if (world_ == 0 || binding.owner.world != world_) return Result::WrongWorld;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        auto& entry = entries_[i];
        if (entry.incarnation != binding) continue;
        if (entry.state != State::Revoked) entry.state = State::Retiring;
        return Result::Ok;
    }
    return Result::StaleBinding;
}
Result Registry::Revoke(const BindingIncarnation& binding) noexcept {
    if (world_ == 0 || binding.owner.world != world_) return Result::WrongWorld;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        auto& entry = entries_[i];
        if (entry.incarnation != binding) continue;
        entry.state = State::Revoked;
        return Result::Ok;
    }
    return Result::StaleBinding;
}
Result Registry::RevokeStorageLifetime(std::uint64_t world, std::uint64_t storageLifetime) noexcept {
    if (world_ == 0 || world != world_) return Result::WrongWorld;
    if (storageLifetime == 0) return Result::InvalidIncarnation;
    bool found = false;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        auto& entry = entries_[i];
        if (entry.value.owner.storageLifetime != storageLifetime) continue;
        entry.state = State::Revoked;
        found = true;
    }
    return found ? Result::Ok : Result::MissingResource;
}

Result Registry::FindResource(const ResourceKey& key, const Entry*& output) const noexcept {
    output = nullptr;
    if (Validate(key) != Result::Ok) return Result::InvalidResource;
    if (world_ == 0) return Result::WrongWorld;
    bool stale = false;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        const auto& entry = entries_[i];
        if (entry.value.resource != key) continue;
        if (entry.state == State::Ambiguous) return Result::AmbiguousResource;
        if (entry.state != State::Active) { stale = true; continue; }
        if (output != nullptr) return Result::AmbiguousResource;
        output = &entry;
    }
    if (output != nullptr) return Result::Ok;
    return stale ? Result::StaleBinding : Result::MissingResource;
}
Result Registry::FindBinding(const BindingIncarnation& binding, const Entry*& output) const noexcept {
    output = nullptr;
    if (world_ == 0 || binding.owner.world != world_) return Result::WrongWorld;
    if (binding.registry != registryId_) return Result::StaleBinding;
    for (std::size_t i = 0; i < entryCount_; ++i) {
        const auto& entry = entries_[i];
        if (entry.incarnation != binding) continue;
        if (entry.state == State::Ambiguous) return Result::AmbiguousResource;
        if (entry.state != State::Active) return Result::StaleBinding;
        output = &entry;
        return Result::Ok;
    }
    return Result::StaleBinding;
}

Result Registry::MapElement(const ElementKey& key, ElementMapping& output) const noexcept {
    const ElementKey requested = key; // Permit MapElement(mapping.key, mapping).
    output = {};
    const Entry* entry = nullptr;
    const auto found = FindResource(requested.resource, entry);
    if (found != Result::Ok) return found;
    std::uint32_t reference = 0;
    const auto& value = entry->value;
    const auto encoded = EncodeLocalReference(value.layout, value.localSalt, value.localSlot,
        value.elementCount, requested.ordinal, reference);
    if (encoded != Result::Ok) return encoded;
    output = ElementMapping{requested, entry->incarnation, reference};
    return Result::Ok;
}
Result Registry::MapLink(const LinkKey& key, LinkMapping& output) const noexcept {
    const LinkKey requested = key; // Permit MapLink(mapping.key, mapping).
    output = {};
    const Entry* entry = nullptr;
    const auto found = FindResource(requested.resource, entry);
    if (found != Result::Ok) return found;
    if (requested.ordinal >= entry->value.specialLinkCount) return Result::InvalidOrdinal;
    const LinkKey* registered = nullptr;
    for (std::size_t i = 0; i < entry->linkCount; ++i)
        if (links_[entry->linkBegin + i].ordinal == requested.ordinal) registered = &links_[entry->linkBegin + i];
    if (registered == nullptr || *registered != requested) return Result::LinkMismatch;
    LinkMapping mapping{};
    mapping.key = requested;
    mapping.binding = entry->incarnation;
    auto mapped = MapElement(requested.incoming, mapping.incoming);
    if (mapped != Result::Ok) return mapped;
    mapped = MapElement(requested.outgoing, mapping.outgoing);
    if (mapped != Result::Ok) return mapped;
    output = mapping;
    return Result::Ok;
}
Result Registry::Revalidate(const ElementMapping& mapping) const noexcept {
    const Entry* entry = nullptr;
    const auto found = FindBinding(mapping.binding, entry);
    if (found != Result::Ok) return found;
    ElementMapping current{};
    const auto mapped = MapElement(mapping.key, current);
    if (mapped != Result::Ok) return mapped;
    return current == mapping ? Result::Ok : Result::StaleBinding;
}
Result Registry::Revalidate(const LinkMapping& mapping) const noexcept {
    const Entry* entry = nullptr;
    const auto found = FindBinding(mapping.binding, entry);
    if (found != Result::Ok) return found;
    LinkMapping current{};
    const auto mapped = MapLink(mapping.key, current);
    if (mapped != Result::Ok) return mapped;
    return current == mapping ? Result::Ok : Result::StaleBinding;
}
Result Registry::ExportElement(const BindingIncarnation& binding,
    std::uint32_t localReference, ElementKey& output) const noexcept {
    output = {};
    const Entry* entry = nullptr;
    const auto found = FindBinding(binding, entry);
    if (found != Result::Ok) return found;
    const auto& value = entry->value;
    const auto ordinal = static_cast<std::uint32_t>(localReference & Mask(value.layout.elementBits));
    std::uint32_t expected = 0;
    const auto encoded = EncodeLocalReference(value.layout, value.localSalt, value.localSlot,
        value.elementCount, ordinal, expected);
    if (encoded != Result::Ok || expected != localReference) return Result::InvalidReference;
    output = ElementKey{value.resource, ordinal};
    return Result::Ok;
}

} // namespace aimp::ai_sync::route_identity
