#pragma once
#include "aimp/AISyncOwnership.h"

namespace aimp::ai_sync::execution {
struct OwnershipMemory {
    std::uintptr_t imageBase{};
    void* context{};
    bool (*read)(void*,std::uintptr_t,void*,std::size_t) noexcept{};
};
struct OwnershipBoundaryFailure {
    // Character-relative offset, or 0x10000 + offset for M's navigation N.
    // Zero means no specific field recorded. Value is copied, not authority.
    std::uint32_t offset{}, value{};
};
struct NativeExecutionAnchor {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    ActorLifetime actor{};
    std::uintptr_t cycle{};
    std::uint8_t serial{};
};
[[nodiscard]] bool MatchesNativeExecution(const OwnershipMemory&,const NativeExecutionAnchor&,
    const transactions::Scope&,const transactions::OwnerKey&,const ActorLifetime&) noexcept;
// Read-only exact-build x86 boundary. Caller supplies the stable engine/world
// lifetime. For requireQuiescence, caller additionally pins Character's M Ref
// through this entire read. Empty scheduler lists alone are insufficient.
// This proves a movement handoff, not graph/listener disposal or Character death.
[[nodiscard]] OwnershipCheck InspectMovementBoundary(const OwnershipMemory&,
    const ActorLifetime&,bool requireQuiescence,OwnershipBoundaryFailure* failure=nullptr) noexcept;
// An authoritative paired transition replaces the current native cycle through
// its virtual destructor. It does not require an invented intermediate Idle.
// Caller must independently own the actor, retire competing requests and bind
// the exact Host interaction before invoking the certified pair constructor.
[[nodiscard]] OwnershipCheck InspectInteractionBoundary(const OwnershipMemory&,
    const ActorLifetime&,OwnershipBoundaryFailure* failure=nullptr) noexcept;
// Same cycle replacement contract, with scheduler/navigation quiescence. The
// caller must additionally prove that this exact cycle/serial was produced by
// an admitted Host native instruction in the current full scope and owner.
[[nodiscard]] OwnershipCheck InspectAuthoritativeExecutionBoundary(const OwnershipMemory&,
    const ActorLifetime&,bool requireQuiescence,OwnershipBoundaryFailure* failure=nullptr) noexcept;
}
