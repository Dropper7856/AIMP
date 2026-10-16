#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::ai_sync::combat {

enum StateFlag : std::uint32_t { Present = 1U, Aiming = 2U, AimPoint = 4U };
// These three values are the resolved retail EquipmentController A5/A6/A7
// presentation flags. Primary input A2 and its local firing decisions are absent.
struct State final {
    std::uint32_t revision{}, flags{}, slot{}, descriptor{};
    float aim[3]{}, turnTolerance{};
    std::uint32_t presentationFlags{};
};
struct ShotEvent final {
    std::uint32_t revision{}, flags{};
    State state{};
    std::uint32_t ammoBefore{};
};
static_assert(sizeof(State) == 36U);
static_assert(sizeof(ShotEvent) == 48U);

inline bool Valid(const State& state) noexcept {
    if ((state.flags & ~(Present | Aiming | AimPoint)) != 0U || state.slot > 10U ||
        state.descriptor > 0xFFFFU || state.presentationFlags > 7U ||
        !std::isfinite(state.turnTolerance) || state.turnTolerance < 0.0F ||
        state.turnTolerance > 3.141593F) return false;
    for (const auto value : state.aim)
        if (!std::isfinite(value) || std::abs(value) > 100000.0F) return false;
    if (state.flags == 0U)
        return state.slot == 0U && state.descriptor == 0U &&
            state.presentationFlags == 0U && state.turnTolerance == 0.0F &&
            state.aim[0] == 0.0F && state.aim[1] == 0.0F && state.aim[2] == 0.0F;
    if ((state.flags & Present) == 0U || state.revision == 0U) return false;
    if ((state.flags & AimPoint) == 0U &&
        (state.aim[0] != 0.0F || state.aim[1] != 0.0F || state.aim[2] != 0.0F ||
            state.turnTolerance != 0.0F)) return false;
    return (state.flags & Aiming) == 0U || (state.flags & AimPoint) != 0U;
}
inline bool Valid(const ShotEvent& event) noexcept {
    if (event.flags == 0U)
        return event.revision == 0U && Valid(event.state) && event.state.flags == 0U && event.state.revision == 0U &&
            event.ammoBefore == 0U;
    return event.flags == 1U && event.revision != 0U && Valid(event.state) &&
        (event.state.flags & (Present | AimPoint)) == (Present | AimPoint) &&
        event.ammoBefore > 0U && event.ammoBefore <= 0xFFFFU;
}
inline bool SameValue(const State& a, const State& b) noexcept {
    return a.flags == b.flags && a.slot == b.slot && a.descriptor == b.descriptor &&
        a.aim[0] == b.aim[0] && a.aim[1] == b.aim[1] && a.aim[2] == b.aim[2] &&
        a.turnTolerance == b.turnTolerance && a.presentationFlags == b.presentationFlags;
}

enum class Mode : std::uint8_t { Retail, Host, Client, FrozenClient };
enum class ApplyResult : std::uint8_t { Applied, AlreadyMatched, Deferred, Failed };
struct Observation final {
    std::uintptr_t character{}; // Local only: root remaps the NPC identity/birth.
    std::uint32_t runtimeId{};
    ShotEvent shot{};
};
struct ReplayCursor final {
    std::uint32_t revision{};
    bool invoked{}, failed{};
};
struct Counters final {
    std::uint64_t captured{}, dropped{}, decisionsBlocked{}, stateApplied{}, shotsApplied{};
    std::uint64_t deferred{}, failed{}, aimSubmitted{}, aimCancelled{};
};
// This predicate must use generation/lifetime-safe published data; a retail
// equipment update may run on a worker. Never consult the engine Actor array.
using NpcPredicate = bool(*)(std::uintptr_t character) noexcept;
bool Initialize(std::uintptr_t base, std::string& error);
bool Install(HookManager& hooks, std::string& error);
void SetNpcPredicate(NpcPredicate predicate) noexcept;
void SetMode(Mode mode) noexcept;
void ResetObservations() noexcept;
bool TryPopObservation(Observation& observation) noexcept;

// The following methods require the engine thread and root's session/world/
// authority-birth checks. Capture seeds revision=1; root stamps its own revision.
bool Capture(std::uintptr_t character, State& state) noexcept;
ApplyResult ApplyState(std::uintptr_t character, std::uint32_t runtimeId,
    const State& state, std::uint64_t expiresAt) noexcept;
// Admit exactly one Host-selected physical shot. Native raycasts/equipment
// update execute it subsequently; this method never invokes the BT, E0 firing
// scheduler, Primary input A2, local cooldown or random burst selection.
ApplyResult ApplyShot(std::uintptr_t character, std::uint32_t runtimeId,
    const ShotEvent& event, ReplayCursor& cursor, std::uint64_t expiresAt) noexcept;
void Cancel(std::uintptr_t character) noexcept;
void Pump(std::uint64_t now) noexcept;
void Reset(bool cancelNative) noexcept;
Counters Snapshot() noexcept;

} // namespace aimp::ai_sync::combat
