#pragma once

#include "aimp/AISyncTransactions.h"
#include <array>

namespace aimp::ai_sync::execution {
struct ActorLifetime {
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, nativeGeneration{};
    std::uint64_t creationSerial{}; // Qualified local construction, never a wire identity.
    bool operator==(const ActorLifetime&) const noexcept = default;
};
struct OwnershipToken {
    transactions::Scope scope{};
    transactions::OwnerKey owner{};
    ActorLifetime actor{};
    std::uint64_t serial{}; // Local, never transmitted or reused.
};
enum class OwnershipCheck : std::uint8_t { Ready, Waiting, Reject, Unknown };
enum class OwnershipPhase : std::uint8_t { Draining, Owned, Retiring, Restoring, Fenced, Released };
enum class OwnershipResult : std::uint8_t {
    Ready, Waiting, Rejected, WrongScope, StaleOwner, Busy, Capacity, Unknown, Reentrant
};
// All callbacks are engine-thread-only and borrowed. Ready from drain means
// every legacy writer of this capability has been fenced AND its native work
// retired at a safe boundary. It is not a network assertion or a Stop ACK.
// nativeReleased must prove Ref/worker/callback quiescence for this exact token.
// restore re-enables only the legacy capability, never an autonomous Client BT.
struct OwnershipAdapter {
    void* context{};
    OwnershipCheck (*resolve)(void*, const transactions::Scope&, const transactions::OwnerKey&, ActorLifetime&) noexcept{};
    OwnershipCheck (*drain)(void*, const OwnershipToken&) noexcept{};
    OwnershipCheck (*nativeReleased)(void*, const OwnershipToken&) noexcept{};
    OwnershipCheck (*restore)(void*, const OwnershipToken&) noexcept{};
    // Read-only local preflight, before installing a Draining token/fencing
    // legacy writers. Waiting leaves the existing executor operational (e.g.
    // a pair still needs its preparation/outro serviced). It grants no lease.
    OwnershipCheck (*prepare)(void*, const transactions::Scope&, const transactions::OwnerKey&,
        const ActorLifetime&) noexcept{};
    // Copied destruction-return proof already exists. Must inspect only owned
    // request/route ledgers, never the destroyed Character or restore its brain.
    OwnershipCheck (*destroyedReleased)(void*, const OwnershipToken&) noexcept{};
};
// No allocation, eviction, timers, inferred success or implicit takeover.
// A local Request is required; decoding a Begin/Submit never grants ownership.
// Retiring/faulted entries keep legacy writers fenced until proven released.
// Values do not pin Character storage. The embedding adapter owns lifetime
// through callbacks; coordinator destruction requires external quiescence.
class OwnershipCoordinator final {
public:
    static constexpr std::size_t kCapacity = 128;
    explicit OwnershipCoordinator(OwnershipAdapter adapter = {}) noexcept : adapter_(adapter) {}
    OwnershipCoordinator(const OwnershipCoordinator&) = delete;
    OwnershipCoordinator& operator=(const OwnershipCoordinator&) = delete;
    [[nodiscard]] OwnershipResult CanConfigure(const transactions::Scope&, std::uint32_t nativeGeneration) const noexcept;
    [[nodiscard]] OwnershipResult Configure(const transactions::Scope&, std::uint32_t nativeGeneration) noexcept;
    [[nodiscard]] OwnershipResult Request(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] OwnershipResult Retry(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] OwnershipResult Retire(const transactions::OwnerKey&) noexcept;
    void RetireAll() noexcept;
    // Lifetime revocation is allowed even inside a native callback. It can
    // only close permission, never grant ownership or invoke cleanup. A zero
    // creationSerial is a conservative revocation of matching address/runtime/
    // generation; it is never a wildcard for acquisition or native cleanup.
    [[nodiscard]] bool InvalidateActor(const ActorLifetime&) noexcept;
    // Exact original Character destruction has RETURNED. A full nonzero local
    // incarnation is mandatory; invalidation, timeout and world change alone
    // are not destruction evidence. May only retire an existing token.
    void ConfirmDestroyed(const ActorLifetime&) noexcept;
    void ReleaseDestroyed() noexcept;
    [[nodiscard]] OwnershipResult Release(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] bool ResolveOwned(const transactions::Scope&, const transactions::OwnerKey&, ActorLifetime&,
        bool cleanup = false) noexcept;
    [[nodiscard]] OwnershipResult ResolveBinding(const transactions::Scope&, const transactions::OwnerKey&, ActorLifetime&,
        bool cleanup = false) noexcept;
    [[nodiscard]] bool AllowsLegacy(const ActorLifetime&) const noexcept;
    [[nodiscard]] bool Find(const transactions::OwnerKey&, OwnershipToken&, OwnershipPhase&) const noexcept;
    [[nodiscard]] bool Read(std::size_t index, OwnershipToken&, OwnershipPhase&) const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept { return count_; }
    [[nodiscard]] bool InTransition() const noexcept { return busy_; }
private:
    struct Entry { OwnershipToken token{}; OwnershipPhase phase{OwnershipPhase::Released}; bool destroyed{}; };
    [[nodiscard]] Entry* FindActor(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] const Entry* FindActor(const transactions::OwnerKey&) const noexcept;
    [[nodiscard]] OwnershipResult Drain(Entry&) noexcept;
    [[nodiscard]] OwnershipResult Revalidate(Entry&) noexcept;
    [[nodiscard]] OwnershipResult Finish(Entry&, OwnershipCheck, OwnershipPhase) noexcept;
    const OwnershipAdapter adapter_;
    std::array<Entry, kCapacity> entries_{};
    transactions::Scope scope_{};
    std::uint32_t nativeGeneration_{};
    std::size_t count_{};
    std::uint64_t serial_{}, revocation_{};
    bool configured_{}, busy_{};
};
} // namespace aimp::ai_sync::execution
