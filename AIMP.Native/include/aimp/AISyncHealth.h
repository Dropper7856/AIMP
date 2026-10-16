#pragma once

#include <cstdint>
#include <cmath>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::health {

enum StateFlag : std::uint32_t { Present = 1U };
inline constexpr std::uint32_t CommitOnly = 2U;

// Portable absolute Host observation. It is not a damage instruction and never
// proves that a paired animation or a death graph has finished.
struct State final {
    std::uint32_t revision{}, flags{}, hp{}, healthState{};
};
static_assert(sizeof(State) == 16U);

constexpr bool Valid(const State& state) noexcept {
    if (state.flags == 0U)
        return state.revision == 0U && state.hp == 0U && state.healthState == 0U;
    return state.flags == Present && state.revision != 0U && state.hp <= 0xFFFFU &&
        state.healthState <= 4U && ((state.hp == 0U) == (state.healthState == 4U));
}
constexpr bool SameValue(const State& left, const State& right) noexcept {
    return left.flags == right.flags && left.hp == right.hp && left.healthState == right.healthState;
}
constexpr bool Terminal(const State& state) noexcept {
    return state.flags == Present && state.hp == 0U && state.healthState == 4U;
}

// Process-local observation only. Remap through the participant peer or the
// source NPC's portable identity/incarnation before sending it over the wire.
struct Source final {
    std::uintptr_t character{};
    std::uint32_t runtimeId{};
};
enum class ApplyResult : std::uint8_t { Applied, AlreadyMatched, Deferred, Failed };
struct DamageEvent final {
    std::uint32_t revision{}, flags{}, damage{}, descriptor{};
    std::uint32_t flag3{};
    std::uint32_t flag4{}, flag5{}, flag6{}, flag8{}, flag9{};
    std::uint32_t hpBefore{}, healthStateBefore{};
    State result{};
};
static_assert(sizeof(DamageEvent) == 64U);
inline bool Valid(const DamageEvent& event) noexcept {
    if (event.flags == 0U)
        return event.revision == 0U && event.damage == 0U && event.descriptor == 0U &&
            event.flag3 == 0U && event.flag4 == 0U && event.flag5 == 0U &&
            event.flag6 == 0U && event.flag8 == 0U && event.flag9 == 0U &&
            event.hpBefore == 0U && event.healthStateBefore == 0U && SameValue(event.result, {}) &&
            event.result.revision == 0U;
    if (event.flags != Present && event.flags != (Present | CommitOnly)) return false;
    if ((event.flags & CommitOnly) != 0U && (event.damage != 0U || event.descriptor != 0U ||
        event.flag3 != 0U || event.flag4 != 0U || event.flag5 != 0U || event.flag6 != 0U ||
        event.flag8 != 0U || event.flag9 != 0U)) return false;
    return event.revision != 0U && event.damage <= 0xFFFFU &&
        event.flag3 <= 0xFFU &&
        event.flag4 <= 0xFFU && event.flag5 <= 0xFFU && event.flag6 <= 0xFFU &&
        event.flag8 <= 0xFFU && event.flag9 <= 0xFFU && event.hpBefore <= 0xFFFFU &&
        event.healthStateBefore <= 4U &&
        ((event.hpBefore == 0U) == (event.healthStateBefore == 4U)) &&
        event.result.flags == Present && Valid(event.result);
}
struct DamageObservation final {
    std::uintptr_t character{};
    std::uint32_t runtimeId{};
    Source source{};
    DamageEvent event{};
};
// One cursor per replicated Actor birth. It records invocation separately from
// convergence, so a deferred postcondition can never replay a hit a second time.
struct DamageReplayCursor final {
    std::uint32_t revision{};
    bool invoked{};
};
struct Counters final {
    std::uint64_t captured{}, captureRefused{}, commits{}, applied{}, alreadyMatched{};
    std::uint64_t deferred{}, failed{}, revivalRefused{}, lifetimeRejected{};
    std::uint64_t damageCaptured{}, damageDropped{}, damageReplayed{}, damageRejected{}, commitsResolved{};
    std::uint64_t nativeDamageBlocked{};
};

// All mutation/capture functions run on the certified engine thread, after the
// caller validates AISync role, world/session and exact Actor incarnation.
bool Initialize(std::uintptr_t base, std::string& error);
bool InstallDamageObserver(HookManager& hooks, std::string& error);
// The caller's lock-free ownership predicate returns false for an unleased
// Client NPC damage mutation. Authoritative health replays bypass this guard.
using NativeDamageGuard = bool(*)(std::uintptr_t character, std::uintptr_t source) noexcept;
void SetNativeDamageGuard(NativeDamageGuard guard) noexcept;
using PairedDamagePredicate = bool(*)(std::uintptr_t character, std::uintptr_t source) noexcept;
void SetPairedDamagePredicate(PairedDamagePredicate predicate) noexcept;
// Reset on session/world changes as well as transitions to/from Host. An
// in-flight worker observation retains its old epoch and cannot cross reset.
void SetObservingHost(bool enabled) noexcept;
void ResetObservations() noexcept;
bool TryPopDamageObservation(DamageObservation& observation) noexcept;
// Observe the actual Host commit when no common-damage call or paired outcome
// already owns it. This records a native setter, without inventing a hit delta.
void ObserveCommit(std::uintptr_t character, std::uint32_t runtimeId, Source source, const State& before) noexcept;
// Captured observations start at revision 1; the Actor owns change detection
// and the monotonic authoritative revision.
bool Capture(std::uintptr_t character, State& observation) noexcept;
bool CaptureDeathSource(std::uintptr_t character, Source& source) noexcept;
// An already-dead local Character is never resurrected. A different authority
// incarnation must be rebound by the caller before attempting resurrection.
// A nonzero source must include its freshly verified local lifetime token.
ApplyResult Apply(std::uintptr_t character, std::uint32_t expectedRuntimeId,
    const State& authority, Source source = {}) noexcept;
inline ApplyResult ApplySnapshot(const std::uintptr_t character, const std::uint32_t expectedRuntimeId,
    const State& authority, const Source source = {}) noexcept {
    return Apply(character, expectedRuntimeId, authority, source);
}
ApplyResult ApplyDamageEvent(std::uintptr_t character, std::uint32_t expectedRuntimeId,
    const DamageEvent& event, Source source, DamageReplayCursor& cursor) noexcept;
// Call this in AISync's existing 007B4AD0 hook BEFORE paired-specific mapping.
// A true return replaces only this replay's exact Character HP argument with
// the observed Host result; all native notifications still execute normally.
bool ResolveCommit(std::uintptr_t healthContext, std::uint32_t& hp) noexcept;
Counters Snapshot() noexcept;

} // namespace aimp::ai_sync::health
