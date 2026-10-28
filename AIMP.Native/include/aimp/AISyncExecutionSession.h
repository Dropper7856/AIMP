#pragma once

#include "aimp/AISyncExecutionCodec.h"
#include "aimp/AISyncActionCorrelation.h"
#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncTransactionExecutor.h"

namespace aimp::ai_sync::execution {
// Bounded consumer callback: engine-thread copied values only, no endpoint reentry.
using WaitTrace = void(*)(const transactions::Record&, std::uint64_t elapsedMs,
    std::uint32_t reason, const transactions::ExecutionDisposition&) noexcept;
void SetWaitTrace(WaitTrace) noexcept;
// Qualified engine-thread endpoint. Transport authentication/current binding
// are supplied by the coordinator; a decoded batch alone cannot enable it.
// No shadow record promotion, automatic opt-in or legacy-owner takeover occurs.
// Diagnostic: the oldest order this Client still holds behind a native
// dependency, and the longest such hold observed. Reason 1 is the ordered
// ledger (an earlier order of the same actor); 2..7 see NativeRequestAdapter.
struct WaitReport {
    std::uint32_t waiting{}, oldestKind{}, oldestOpcode{}, oldestAdmission{}, oldestReason{};
    std::uint64_t oldestSeq{}, oldestAgeMs{};
    std::uint32_t maxKind{}, maxOpcode{}, maxReason{};
    std::uint64_t maxSeq{}, maxAgeMs{};
    std::uint64_t transitionPlacements{}; // Host pose applied at backstage/frontstage.
    std::uint64_t lifecycleEvents{};      // Host native lifecycle calls replayed.
};
class ExecutionSession final {
public:
    explicit ExecutionSession(transactions::Limits limits = {}) noexcept;
    [[nodiscard]] bool Initialize(std::uintptr_t base, bool exactBuildValidated,
        Permission permission, ResolveActor resolver, void* context, DependencyGate = nullptr, ResolveActor lifecycleResolver = nullptr) noexcept;
    [[nodiscard]] transactions::Result Configure(const transactions::Scope&,
        transactions::ExecutionBarrier = transactions::ExecutionBarrier::RequireReleased) noexcept;
    [[nodiscard]] transactions::Result FenceOwner(const transactions::OwnerKey&) noexcept;
    [[nodiscard]] transactions::Result Checkpoint(const transactions::Scope&, std::uint64_t) noexcept;
    // Caller authenticates the Host and passes the CURRENT scope, never one
    // taken only from received bytes. Validates the whole batch before effects.
    // Admission remains per operation; a batch is not an atomic engine action.
    [[nodiscard]] bool Handle(std::span<const std::uint8_t> bytes, const transactions::Scope& current,
        bool authenticatedHost, std::span<Receipt> receipts, std::size_t& count) noexcept;
    [[nodiscard]] bool Retry(const transactions::Record& exact, std::uint64_t batchId,
        const transactions::Scope& current, bool authenticatedHost, Receipt&) noexcept;
    [[nodiscard]] NativeRequestStatus Inspect(const transactions::Record&) noexcept;
    [[nodiscard]] NavigationOwnership IdentifyNavigation(std::uintptr_t n,std::uintptr_t b,NavigationBinding& out,
        bool currentLifetime = true) noexcept {
        return configured_ ? native_.IdentifyNavigation(n,b,out,currentLifetime&&!retired_) : NavigationOwnership::Unowned;
    }
    // Exact retained evidence only; this neither retries nor grants ownership.
    [[nodiscard]] bool IsWaiting(const transactions::Record&) const noexcept;
    [[nodiscard]] action_correlation::Readiness ActionReadiness(const action_correlation::Reference&,
        const transactions::OwnerKey&, bool admittedHistory = false) noexcept;
    [[nodiscard]] std::size_t ActionBlockingRequests(const action_correlation::Reference&,
        const transactions::OwnerKey&, std::uintptr_t character) const noexcept;
    [[nodiscard]] std::size_t ActiveRequests() const noexcept { return native_.ActiveCount(); }
    [[nodiscard]] std::size_t ActorRequests(std::uintptr_t c) const noexcept { return native_.ActorRequests(c); }
    [[nodiscard]] std::size_t RetainedRecords() const noexcept { return executor_.RecordCount(); }
    [[nodiscard]] std::uint64_t AcceptedThrough() const noexcept {return acceptedThrough_;}
    [[nodiscard]] bool OperationInProgress() const noexcept { return busy_; }
    [[nodiscard]] WaitReport Waits() const noexcept;
    // Engine thread, per frame: Host-guided Moves follow the Host path points.
    void Steer() noexcept { if (!busy_ && configured_ && initialized_ && !retired_) native_.SteerMovements(); }
    [[nodiscard]] bool BeginRetirement() noexcept;
    void ConfirmActorDestroyed(const LocalActorBinding&) noexcept;
    [[nodiscard]] RetirementProgress DrainRetirement(RetirementGuard, void* context, std::size_t budget = 4) noexcept;
    [[nodiscard]] RetirementProgress DrainActorRetirement(const LocalActorBinding&, RetirementGuard, void* context) noexcept;
    [[nodiscard]] bool Retired() const noexcept { return retired_; }
    [[nodiscard]] bool RetirementComplete() const noexcept { return retired_ && retirementComplete_; }
    [[nodiscard]] RetirementProgress RetirementState() const noexcept { return native_.RetirementState(); }
    [[nodiscard]] bool OwnerReleased(const transactions::OwnerKey& owner) const noexcept {
        return !busy_ && configured_ && native_.ActiveCount(owner) == 0 &&
            (retired_ || (!executor_.HasNativeOwnership(owner) && !executor_.HasWaiting(owner)));
    }
private:
    friend struct ExecutionSessionTestAccess;
    static transactions::Dependency Check(void*, const transactions::Record&);
    static transactions::NativeOutcome Invoke(void*, const transactions::Record&);
    struct WaitProbe { std::uint64_t seq{}, start{}; std::uint32_t kind{}, opcode{}, admission{}, reason{}, tracedReasons{}; };
    void NoteWait(const transactions::Record&, const transactions::ExecutionDisposition&) noexcept;
    static Receipt MakeReceipt(const transactions::Record&, std::uint64_t, const transactions::ExecutionDisposition&) noexcept;
    NativeRequestAdapter native_;
    transactions::TransactionExecutor executor_;
    transactions::Scope scope_{};
    Permission permission_{};
    void* context_{};
    bool initialized_{}, configured_{}, busy_{};
    bool retired_{}, retirementComplete_{};
    std::uint64_t acceptedThrough_{};
    std::array<WaitProbe, 32> waits_{};
    WaitReport worst_{};
};
} // namespace aimp::ai_sync::execution
