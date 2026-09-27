#pragma once

#include "aimp/WorldReplicationTypes.h"
#include <unordered_map>
#include <unordered_set>

namespace aimp::world_actions {

enum class ActionClass : std::uint8_t {
    GenericRoot, TargetedRoot, PayloadRoot, NeedsAdapter, LocalOnly, Unsafe, Unknown, Count
};

// ROOT v9: the historical DetachedCallback bit denotes a context transferred
// semantically and reconstructed locally. It NEVER authorizes a null parent.
inline constexpr std::uint8_t kPublicRootFlagExternalInvocation = 1U << 0U;
inline constexpr std::uint8_t kPublicRootFlagDetachedCallback = 1U << 1U;
inline constexpr std::uint8_t kPublicRootFlagAwaitAuthority = 1U << 2U;
inline constexpr std::uint8_t kPublicRootKnownFlags =
    kPublicRootFlagExternalInvocation | kPublicRootFlagDetachedCallback | kPublicRootFlagAwaitAuthority;

// Causality is evaluated at the semantic boundary being captured, not by
// blacklisting every non-World ancestor. A known Player/AI/UI/AIMP cause may
// legitimately produce World work. This direct classifier describes one edge;
// the runtime capture may walk a bounded retained ancestry because retail often
// inserts internal World WorkItems before the first replayable public Method.
// An all-World chain remains a continuation unless ROOT lineage identifies it.
enum class CausalBoundaryClass : std::uint8_t {
    CleanWorldRoot,
    LocalToWorldBoundary,
    WorldContinuation,
    LocalEffect,
    Unknown,
};

[[nodiscard]] constexpr CausalBoundaryClass ClassifyCausalBoundary(
    const WorldDomain current, const bool hasParent,
    const WorldDomain parent = WorldDomain::Unknown) noexcept {
    if (current == WorldDomain::Unknown) return CausalBoundaryClass::Unknown;
    if (current != WorldDomain::World) return CausalBoundaryClass::LocalEffect;
    if (!hasParent) return CausalBoundaryClass::CleanWorldRoot;
    if (parent == WorldDomain::Unknown) return CausalBoundaryClass::Unknown;
    if (parent == WorldDomain::World) return CausalBoundaryClass::WorldContinuation;
    return CausalBoundaryClass::LocalToWorldBoundary;
}

// Retail startup 01152070/011520A0 initializes DAT_0174DAF0/DAT_0174DAFC
// from these names. Values independently derived with CathodeLib ShortGuidUtils.
inline constexpr std::uint32_t kActivateBehavior = 0xA4737650U;
inline constexpr std::uint32_t kDeactivateBehavior = 0x90BC198AU;
[[nodiscard]] constexpr bool ForbiddenPublicInput(std::uint32_t input) noexcept {
    return input == kActivateBehavior || input == kDeactivateBehavior;
}

// Precomputed structural contract, not a claim of callback independence.
// A distinct execution target must be an exact endpoint of THIS public port.
// Extended invocations also require all direct endpoints to be World-domain.
class PublicRootIndex final {
    struct PortKey {
        WorldEntityId owner{};
        std::uint32_t input{};
        bool operator==(const PortKey&) const noexcept = default;
    };
    struct PortHash {
        std::size_t operator()(const PortKey& key) const noexcept {
            const auto value = key.owner.high ^ key.owner.low;
            return static_cast<std::size_t>(value ^ (value >> 32U) ^ key.input);
        }
    };
    struct TargetKey {
        PortKey port{};
        WorldEntityId target{};
        bool operator==(const TargetKey&) const noexcept = default;
    };
    struct TargetHash {
        std::size_t operator()(const TargetKey& key) const noexcept {
            const auto value = key.target.high ^ key.target.low;
            return PortHash{}(key.port) ^
                (static_cast<std::size_t>(value ^ (value >> 32U)) * 0x9E3779B1U);
        }
    };
    struct Port {
        WorldEntityId interfaceId{};
        bool valid{true};
        bool directWorldOnly{true};
        bool methodOnly{true};
    };
public:
    void Add(const WorldCompositePublicInputDescriptor& bridge, bool ownerWorld,
             bool interfaceWorld, bool targetWorld, bool localEffects = false) {
        if (!ownerWorld || !bridge.ownerCompositeInstance.Valid() ||
            !bridge.interfaceVariableEntity.Valid() || !bridge.internalTargetEntity.Valid() ||
            bridge.publicInputShortGuid == 0U || bridge.internalTargetInputShortGuid == 0U) return;
        const PortKey key{bridge.ownerCompositeInstance, bridge.publicInputShortGuid};
        auto [found, inserted] = ports_.try_emplace(key);
        auto& port = found->second;
        if (inserted) port.interfaceId = bridge.interfaceVariableEntity;
        port.valid = port.valid && port.interfaceId == bridge.interfaceVariableEntity &&
            !localEffects &&
            !ForbiddenPublicInput(bridge.publicInputShortGuid) &&
            !ForbiddenPublicInput(bridge.internalTargetInputShortGuid);
        port.directWorldOnly = port.directWorldOnly && interfaceWorld && targetWorld;
        port.methodOnly = port.methodOnly && bridge.noTypedData;
        if (interfaceWorld) targets_.insert({key, bridge.interfaceVariableEntity});
        if (targetWorld) targets_.insert({key, bridge.internalTargetEntity});
    }
    [[nodiscard]] bool Known(const WorldEntityId& owner, std::uint32_t input) const noexcept {
        const auto found = ports_.find({owner, input});
        return found != ports_.end() && found->second.valid;
    }
    [[nodiscard]] bool Allows(const WorldEntityId& owner, std::uint32_t input,
                             const WorldEntityId& target, bool extended,
                             bool requireMethod = false) const noexcept {
        if (!target.Valid()) return false;
        const PortKey key{owner, input};
        const auto found = ports_.find(key);
        if (found == ports_.end() || !found->second.valid) return false;
        // Calls whose sender-local invocation context is intentionally detached
        // (external or callback-boundary ROOTs) are only portable for public
        // Methods. Typed public ports stay NeedsAdapter until a payload codec is
        // independently certified.
        if (requireMethod && !found->second.methodOnly) return false;
        if (target == owner && !extended) return true;
        return found->second.directWorldOnly &&
            (target == owner || targets_.contains({key, target}));
    }
    void Clear() noexcept { ports_.clear(); targets_.clear(); }
private:
    std::unordered_map<PortKey, Port, PortHash> ports_;
    std::unordered_set<TargetKey, TargetHash> targets_;
};
} // namespace aimp::world_actions
