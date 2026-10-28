#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aimp::ai_sync::route_identity {

inline constexpr std::size_t kMaxLevelBytes = 96;
inline constexpr std::size_t kMaxBindings = 64;
inline constexpr std::size_t kMaxLinks = 1024;
inline constexpr std::uint32_t kNativeFormatVersion = 7;
using ImmutableDigest = std::array<std::uint8_t, 32>;

// Value types, NOT native ABI layouts or a wire codec. The caller supplies the
// exact canonical level ID (uppercase ASCII letters/digits/_/-; zero padded),
// state and SHA-256 of the associated blob BEFORE native initialization. The
// native bucket hash/header tuple alone is never a resource identity. No file
// reading, digest computation, canonicalization, or provenance discovery here.
struct ResourceKey {
    std::array<char, kMaxLevelBytes> level{};
    std::uint32_t state{};
    std::uint32_t formatVersion{kNativeFormatVersion};
    ImmutableDigest immutableDigest{};
    std::array<std::uint32_t, 3> headerTuple{};
    bool operator==(const ResourceKey&) const noexcept = default;
};
struct ElementKey {
    ResourceKey resource{};
    std::uint32_t ordinal{};
    bool operator==(const ElementKey&) const noexcept = default;
};
// Native context words are opaque and ORDERED. They check consistency only;
// they never select a resource or replace the resource+ordinal+endpoints key.
struct ContextPair {
    std::uint32_t first{};
    std::uint32_t second{};
    bool operator==(const ContextPair&) const noexcept = default;
};
struct LinkKey {
    ResourceKey resource{};
    std::uint32_t ordinal{};
    ElementKey incoming{};
    ElementKey outgoing{};
    ContextPair context{};
    bool operator==(const LinkKey&) const noexcept = default;
};

// Original x86 image SHA256 00d6bb846ac5a78916baf363eb148aa255a23bf216a271cfd23a0ece3c368364,
// base 00400000: 00440DF0 derives p/t/saltBits; 0043DE20 and 00448280 encode
// i | (s << p) | (salt << (p+t)); 0043DC70 validates. Phase3 identity I3/I9.
// These VAs document provenance, never callable addresses in this library.
// Capacities imply ceil(log2) widths, saltBits=min(31,32-p-t), >=10.
struct HandleLayout {
    std::uint32_t elementBits{}; // p, W+50
    std::uint32_t slotBits{};    // t, W+4C
    std::uint32_t saltBits{};    // W+48
    std::uint32_t slotCapacity{};
    std::uint32_t elementCapacity{};
    bool operator==(const HandleLayout&) const noexcept = default;
};
enum class Result : std::uint8_t {
    Ok, InvalidResource, InvalidLayout, InvalidOrdinal, InvalidSalt,
    InvalidSlot, InvalidIncarnation, UnverifiedBinding, WrongWorld,
    StaleBinding, MissingResource, DuplicateBinding, AmbiguousResource,
    DuplicateLink, AmbiguousContext, LinkMismatch, CapacityExceeded,
    IncarnationExhausted, InvalidReference
};

// Locally assigned opaque IDs, never Host handles, pointers or ownership.
// load is a strictly increasing load-event ID within a world. Bind all slots
// of an event together before starting another event. A reload MUST get a new
// load and storageLifetime, even when native W/salt/raw refs repeat.
struct LoadIncarnation {
    std::uint64_t world{};
    std::uint64_t load{};
    std::uint64_t localMesh{};
    std::uint64_t storageLifetime{};
    bool operator==(const LoadIncarnation&) const noexcept = default;
};
struct BindingIncarnation {
    LoadIncarnation owner{};
    std::uint64_t serial{};
    // Process-local, never reused, distinguishes independent/recreated
    // registries even if caller-assigned world/load IDs accidentally repeat.
    // Mappings are transient local values; never persist or transmit tickets.
    std::uint64_t registry{};
    bool operator==(const BindingIncarnation&) const noexcept = default;
};
struct CallerVerifiedBinding {
    ResourceKey resource{};
    LoadIncarnation owner{};
    HandleLayout layout{};
    std::uint32_t localSlot{};
    std::uint32_t localSalt{};
    std::uint32_t elementCount{};
    // Number of certified special-link lookup entries supplied with this
    // binding. Zero can describe an ordinary-only capability subset; it does
    // not certify absence of authored special links in the native blob.
    std::uint32_t specialLinkCount{};
    // Both default false. Set only after an external loader adapter has
    // associated these exact pre-init bytes with the successfully loaded mesh,
    // validated local slot/count/layout/salt/link records, and established a
    // real storage lifetime/quiescence protocol. A W Ref alone is insufficient
    // for manager-owned flags=0 blob storage (Phase3 I5/I10). These are caller
    // attestations; accepting them is NOT native verification or admission.
    bool resourceAndLocalMeshVerified{};
    bool storageLifetimeVerified{};
};
struct ElementMapping {
    ElementKey key{};
    BindingIncarnation binding{};
    std::uint32_t localReference{};
    bool operator==(const ElementMapping&) const noexcept = default;
};
struct LinkMapping {
    LinkKey key{};
    BindingIncarnation binding{};
    ElementMapping incoming{};
    ElementMapping outgoing{};
    bool operator==(const LinkMapping&) const noexcept = default;
};

[[nodiscard]] Result Validate(const ResourceKey& key) noexcept;
[[nodiscard]] Result Validate(const HandleLayout& layout) noexcept;
[[nodiscard]] Result EncodeLocalReference(const HandleLayout& layout,
    std::uint32_t salt, std::uint32_t slot, std::uint32_t elementCount,
    std::uint32_t ordinal, std::uint32_t& output) noexcept;

// Fixed memory/count bounds, no eviction. Retired/revoked/ambiguous records
// remain tombstones until BeginWorld to prevent resurrection/token reuse.
// Engine-thread/external synchronization required. This core never pins or
// reads native memory. The owner MUST retire/revoke BEFORE storage invalidates,
// drain native users at its certified safe point, and hold that protection
// across Revalidate AND native use. Revalidate alone is not a memory lease.
// Native integration must additionally check 0043DC70 / 004417C0, adjacency and
// dynamic availability; neither successful mapping nor a digest certifies them.
class Registry final {
public:
    Registry() noexcept;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

    // world is nonzero and strictly increases for this Registry's lifetime.
    // Invalidates all old mappings before dropping world-local tombstones.
    [[nodiscard]] Result BeginWorld(std::uint64_t world) noexcept;
    void EndWorld() noexcept;
    [[nodiscard]] Result BindCallerVerified(const CallerVerifiedBinding& binding,
        std::span<const LinkKey> links, BindingIncarnation& output) noexcept;
    // First retirement step invalidates immediately, before native cleanup.
    // Revoke completes the value-layer retirement; neither API frees storage.
    [[nodiscard]] Result BeginRetire(const BindingIncarnation& binding) noexcept;
    [[nodiscard]] Result Revoke(const BindingIncarnation& binding) noexcept;
    // Invalidate EVERY binding borrowing the token before its backing storage
    // is freed/reused. Repeated revocation is safe; token reuse is rejected.
    [[nodiscard]] Result RevokeStorageLifetime(std::uint64_t world,
        std::uint64_t storageLifetime) noexcept;

    [[nodiscard]] Result MapElement(const ElementKey& key, ElementMapping& output) const noexcept;
    [[nodiscard]] Result MapLink(const LinkKey& key, LinkMapping& output) const noexcept;
    [[nodiscard]] Result Revalidate(const ElementMapping& mapping) const noexcept;
    [[nodiscard]] Result Revalidate(const LinkMapping& mapping) const noexcept;
    // Host export/local observation must supply the original incarnation; raw
    // salt1 refs alone cannot be revalidated after a reload or world change.
    [[nodiscard]] Result ExportElement(const BindingIncarnation& binding,
        std::uint32_t localReference, ElementKey& output) const noexcept;

private:
    enum class State : std::uint8_t { Active, Retiring, Revoked, Ambiguous };
    struct Entry {
        CallerVerifiedBinding value{};
        BindingIncarnation incarnation{};
        State state{State::Active};
        std::size_t linkBegin{};
        std::size_t linkCount{};
    };
    [[nodiscard]] Result FindResource(const ResourceKey& key, const Entry*& output) const noexcept;
    [[nodiscard]] Result FindBinding(const BindingIncarnation& binding, const Entry*& output) const noexcept;
    std::array<Entry, kMaxBindings> entries_{};
    std::array<LinkKey, kMaxLinks> links_{};
    std::size_t entryCount_{};
    std::size_t linkCount_{};
    std::uint64_t world_{};
    std::uint64_t lastWorld_{};
    std::uint64_t lastLoad_{};
    std::uint64_t lastSerial_{};
    std::uint64_t registryId_{};
};

} // namespace aimp::ai_sync::route_identity
