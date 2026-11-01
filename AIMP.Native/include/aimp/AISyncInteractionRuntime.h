#pragma once

#include "aimp/AISyncInteraction.h"

namespace aimp::ai_sync {
struct State;

namespace interactions {

// Scheduler veto for an admitted source whose exact graph is still attached
// and has no native end request. E6/stage 6 and victim end/replacement alone
// must not disable that veto. Flags|6 permits native cleanup, but does NOT
// prove graph disposal, callback quiescence or resourcesReleased.
// This is a read, not a pin: caller holds the published world lifetime and
// calls on the engine thread before allowing a source transition.
// Unknown reads keep the veto for this admitted anchor; they never prove end.
[[nodiscard]] Check CheckSourceRetention(const Memory&, const LocalParticipant&,
    const PairState& admitted, std::uintptr_t expectedCycle) noexcept;
[[nodiscard]] bool RetainsNativeSource(const Memory&, const LocalParticipant&,
    const PairState& admitted, std::uintptr_t expectedCycle) noexcept;
// Exact admitted source/graph still attached AND native End_Action flags6.
// Authorizes a retail exit request only; never implies resourcesReleased.
[[nodiscard]] Check CheckSourceExit(const Memory&, const LocalParticipant&,
    const PairState& admitted, std::uintptr_t expectedCycle) noexcept;

// Engine-thread bridge from the authenticated State carrier to the existing
// retail adapter. No wire pointers, placement, timer, graph pump or producer.
// Permission and ReleaseProof retain their NativePairAdapter contracts; the
// parent must prove both exclusive local owners and actual native quiescence.
// Worker notifications must be queued with their original key/receipt serial,
// actor, cycle and cycle serial, then drained through Observe on this thread.
class Runtime final {
public:
    static constexpr std::size_t kCapacity = NativePairAdapter::kCapacity;
    [[nodiscard]] bool Initialize(std::uintptr_t imageBase, bool exactBuildValidated,
        Entrypoints, Permission, void* context) noexcept;
    // The parent authenticates this association. The carrier must echo its
    // registration-time full session/authority seal and Host command ID.
    // Rebinding a consumer requires a new Host authority to reject old pairs.
    [[nodiscard]] Result Configure(const transactions::Scope&, std::uint64_t sessionCookie,
        std::uint32_t worldGeneration) noexcept;
    // Qualified engine world change only. Revokes reservations, NOT native
    // resources. Old callbacks remain stale by scope and monotonic receipt ID.
    [[nodiscard]] bool ResetForWorld(std::uint32_t currentNativeGeneration) noexcept;
    // Source is an NPC; player victims require State v13's registration-time
    // seal, a matching portable incarnation and exact local construction.
    // Permission must prove the association and both exclusive reservations.
    // Waiting can be retried before entry; any partial native entry is retained
    // by NativePairAdapter and never entered again for this pair.
    [[nodiscard]] Result Submit(const State&, const Binding&, Receipt&) noexcept;
    [[nodiscard]] bool Find(const State&, Receipt&) const noexcept;
    [[nodiscard]] Result Inspect(const Key&, Receipt&) noexcept;
    [[nodiscard]] Result Observe(const Key&, std::uint64_t receiptSerial,
        const LocalParticipant&, std::uintptr_t cycle, std::uint8_t cycleSerial,
        FinishCause, Receipt&) noexcept;
    // Call only after the exact bound Host preparation actually ran locally;
    // ClaimOutcome/admission/HP alone is not this evidence.
    [[nodiscard]] Result OutcomeApplied(const Key&, std::uint64_t receiptSerial,
        const Outcome&, std::uint32_t evidence) noexcept;
    [[nodiscard]] Result Release(const Key&, ReleaseProof, void* context, Receipt&) noexcept;
    void Invalidate(const LocalParticipant&) noexcept;
    // Query before legacy scheduler TLS gates. Parent additionally requires
    // Client, engine thread, stable world and the published authority scope.
    // Does not call Permission: victim liveness is independent of source outro.
    [[nodiscard]] bool RetainSource(std::uintptr_t source, std::uintptr_t cycle,
        const Memory&) noexcept;
private:
    friend struct RuntimeTestAccess;
    struct Entry {
        Command command{};
        Binding binding{};
        PairState ownedSource{};
        bool invalidated{}, sourceRevoked{}, sourceEnded{};
    };
    void CaptureSource(Entry&, const Receipt&) noexcept;
    [[nodiscard]] Entry* FindEntry(const Key&) noexcept;
    [[nodiscard]] bool MatchesCarrier(const State&) const noexcept;
    NativePairAdapter adapter_{};
    std::array<Entry, kCapacity> entries_{};
    std::size_t count_{};
    std::uint64_t sessionCookie_{};
    std::uint32_t worldGeneration_{};
    std::uintptr_t base_{};
    transactions::Scope scope_{};
    bool configured_{}, busy_{};
};
} // namespace interactions
} // namespace aimp::ai_sync
