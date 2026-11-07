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
// Exact local result of admitted NativeIntent21/25 (0064A8A0/0064A930), never a
// Host pose or sampled cycle state. Retail00402310/00402640 set mask0x06 at arrival;
// 00460CF0 consumes mask0x04. A finished/replaced/unreadable cycle owns nothing.
[[nodiscard]] bool IsLiveNativePassage(const OwnershipMemory&,const NativeExecutionAnchor&,
    const transactions::Scope&,const transactions::OwnerKey&,const ActorLifetime&) noexcept;
[[nodiscard]] constexpr bool OrdinaryPassageTransition(std::uint32_t state) noexcept {
    // 77 is the backstage compatibility fallback, guarded only at the two
    // certified scheduler sites after native movement mode5 is established.
    return state==1U||state==2U||state==0x38U||state==0x3BU||state==0x4DU;
}
// An accepted causal replacement releases passage protection. Updates and
// the direct instruction's immediate Complete/Release do not end its cycle.
[[nodiscard]] bool SupersedesNativePassage(const NativeExecutionAnchor&,std::uint64_t sequence,
    std::uint64_t command,const transactions::Record&,transactions::NativeOutcome) noexcept;
// Read-only exact-build x86 boundary. Caller supplies the stable engine/world
// lifetime. For requireQuiescence, caller additionally pins Character's M Ref
// through this entire read. Empty scheduler lists alone are insufficient.
// This proves a movement handoff, not graph/listener disposal or Character death.
[[nodiscard]] OwnershipCheck InspectMovementBoundary(const OwnershipMemory&,
    const ActorLifetime&,bool requireQuiescence,OwnershipBoundaryFailure* failure=nullptr) noexcept;
// Initial local backstage HOLD only (00401520/00401580), never traversal75/76.
// Caller proves the current incarnation, initial placement and absence of a
// replayed Host cycle. Scheduler/navigation quiescence is still mandatory
// after the drain. This grants no permission to synthesize a cycle transition.
[[nodiscard]] OwnershipCheck InspectInitialBackstageBoundary(const OwnershipMemory&,
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
