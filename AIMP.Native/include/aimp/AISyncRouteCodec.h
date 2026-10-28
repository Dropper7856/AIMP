#pragma once

#include "aimp/AISyncRouteIdentity.h"
#include "aimp/AISyncTransactions.h"
#include <vector>

namespace aimp::ai_sync::route {
inline constexpr std::uint32_t kMagic = 0x52534941U; // AISR
inline constexpr std::uint16_t kVersion = 4;
inline constexpr std::size_t kHeaderBytes = 160, kResourceBytes = 148, kNodeBytes = 52;
inline constexpr std::size_t kMaxResources = 8, kMaxNodes = 256;
inline constexpr std::size_t kMaxBytes = kHeaderBytes + kMaxResources * kResourceBytes + kMaxNodes * kNodeBytes;

struct Key {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    std::uint64_t commandId{};
    std::uint32_t parameterRevision{}, routeRevision{};
    // Native compound Move owns distinct primary/companion navigation blocks.
    // Neither result may be admitted as the other part's route.
    std::uint32_t part{};
    bool operator==(const Key&) const noexcept = default;
};
// Installed Host nodes: type0 boundary, type1 special link or type2 corridor.
// Type3 scratch remains unsupported. The vectors are position and the ordered portal
// pair, not destinations for independent Client path queries. Repeated nodes
// and resource ordinals retain their order. No pointers, native refs or salt.
struct Node {
    std::uint32_t type{2}, resourceIndex{}, ordinal{};
    std::array<float, 9> vectors{};
    // Up to four ordered, parameterless native tags (2/3/4/7/8/9/11), low byte
    // first, followed only by zero bytes. No pointers or weak refs cross.
    std::uint32_t markers{};
    bool operator==(const Node&) const noexcept = default;
};
[[nodiscard]] constexpr bool ValidMarkers(std::uint32_t markers) noexcept {
    bool ended=false;
    for(unsigned i=0;i<4;++i) {
        const auto tag=(markers>>(8*i))&255U;
        if(!tag)ended=true;
        else if(ended||(tag!=2&&tag!=3&&tag!=4&&tag!=7&&tag!=8&&tag!=9&&tag!=11))return false;
    }
    return true;
}
enum class Failure : std::uint32_t { None, Unavailable, Unsupported, Capacity, InvalidStream, NativeFault };
struct Graph {
    // Command-scoped terminal route refusal. No graph/native ownership claim.
    Failure failure{};
    Key key{};
    // R+10..24: the two producer auxiliary vectors, retained independently of
    // node coordinates. R+28/2C/30/34: four navigation status words, NOT handles
    // or world epochs. The local R+08 invalidation token is never transported.
    std::array<float, 6> auxiliary{};
    // Actual Host B goal at the route boundary, not a projected Client goal.
    std::array<float, 3> goal{};
    std::array<std::uint32_t, 4> statuses{};
    std::array<route_identity::ResourceKey, kMaxResources> resources{};
    std::array<Node, kMaxNodes> nodes{};
    std::size_t resourceCount{}, nodeCount{};
    bool operator==(const Graph&) const noexcept = default;
};
enum class Result : std::uint8_t {
    Ok, InvalidKey, InvalidGraph, InvalidEncoding, UnsupportedDomain, WrongKey,
    MissingResource, InvalidNativeState, LifetimeLost, LocalInvalidity,
    NativeFault, AlreadyEntered, Reentrant, NotInitialized,
    // Local capture only: native admission/replacement is not yet settled,
    // or this B has already consumed its range. Never encoded as a route.
    Deferred
};
[[nodiscard]] bool ValidKey(const Key&) noexcept;
[[nodiscard]] Result Validate(const Graph&) noexcept;
[[nodiscard]] Result Encode(const Graph&, std::vector<std::uint8_t>&) noexcept;
// Bounded header identity inspection; does not validate/admit graph content.
[[nodiscard]] Result DecodeKey(std::span<const std::uint8_t>, Key&) noexcept;
// expected is supplied by the authenticated command binding, never inferred
// from payload bytes. On failure output remains unchanged.
[[nodiscard]] Result Decode(std::span<const std::uint8_t>, const Key& expected, Graph&) noexcept;
} // namespace aimp::ai_sync::route
