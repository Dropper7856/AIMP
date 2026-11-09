#pragma once

#include "aimp/PlayerHidingPresentation.h"
#include <cstdint>
#include <string>

namespace aimp::player_hiding_presentation {

// These are the aliases bound by Hiding_logic_common's actual CMD_PlayAnimation
// inputs, not clip-path hashes from ANIMATION.PAK.
struct ClipSelection final { std::uint32_t phase{}, kind{}; };
// A retail-accepted exact door edge certifies entry before PLAYER_HIDING.
// First-person entry need not be the NPC scripted cycle sampled on Character.
[[nodiscard]] inline std::uint32_t LocalPhase(std::uint32_t previous, bool sameDoor,
    const State& door, std::uint32_t nativePhase, bool hidden, bool interactionActive) noexcept {
    if (!sameDoor) {
        if (door.phase == 1U && hidden) return 2U;
        if (door.elapsedMilliseconds > 500U) return 0U;
        return door.phase == 1U || door.phase == 3U ? door.phase : 0U;
    }
    if (previous == 3U) return hidden || nativePhase == 3U ? 3U : 0U;
    if (door.phase == 3U) return 0U;
    if (hidden) return 2U;
    if (previous == 1U && door.elapsedMilliseconds <= 5000U &&
        (door.elapsedMilliseconds <= 500U || interactionActive || nativePhase == 1U)) return 1U;
    return 0U;
}
// Native Hiding_logic_common aliases. These identify an edge only when
// first-person hiding is not represented by a Character scripted cycle;
// all actual playback still goes through AnimationSync.
[[nodiscard]] constexpr std::uint32_t ClipToken(std::uint32_t kind, std::uint32_t phase) noexcept {
    if (kind == 1U) return phase == 1U ? 0xCF2A9F36U : phase == 2U ? 0x1C440762U : phase == 3U ? 0x5E642560U : 0U;
    if (kind == 2U) return phase == 1U ? 0x80645982U : phase == 2U ? 0x15691ACEU : phase == 3U ? 0x3FDD3AF4U : 0U;
    return 0U;
}
// Stay concealed until the exit clip is admitted, but do not wait forever if
// streaming or native execution fails.
[[nodiscard]] constexpr bool DeferExitReveal(std::uint32_t phase, bool concealedByUs,
    bool exitPlaying, std::uint64_t elapsedMs) noexcept {
    return phase == 3U && concealedByUs && !exitPlaying && elapsedMs < 1500U;
}
[[nodiscard]] constexpr ClipSelection ClassifyClip(std::uint32_t token) noexcept {
    switch (token) {
    case 0xCF2A9F36U: return {1U, 1U};
    case 0x1C440762U: return {2U, 1U};
    case 0x5E642560U: return {3U, 1U};
    case 0x80645982U: return {1U, 2U};
    case 0x15691ACEU: return {2U, 2U};
    case 0x3FDD3AF4U: return {3U, 2U};
    default: return {};
    }
}
// Capture and occupancy only. AnimationSync owns playback and authored turns.
void Initialize(std::uintptr_t moduleBase, std::uintptr_t nativeIdleTransition = 0U) noexcept;
// Value-only occupancy mailbox. Phase zero releases the hidden movement lock.
bool Publish(std::uintptr_t character, const State&, std::string& error) noexcept;
// Call before movement/turn/pose correction, even without a movement mailbox.
// True only for settled Hidden; entry and exit belong to AnimationSync.
bool PumpOnEngineThread(std::uintptr_t character) noexcept;
void PumpAllOnEngineThread() noexcept;
// Compatibility no-op: this module no longer requests animation streams.
void DrainStreamsOnEngineThread() noexcept;
[[nodiscard]] bool Active(std::uintptr_t character) noexcept;
// Owner's transform captured when it became hidden or started to leave:
// inside the prop, facing out.
[[nodiscard]] bool HiddenPlacement(std::uintptr_t character, std::array<float, 3>& position, float& yaw) noexcept;
// Compatibility queries return false: no legacy clip or forced exit heading.
[[nodiscard]] bool ClipPlaying(std::uintptr_t character) noexcept;
[[nodiscard]] bool HiddenExitHeading(std::uintptr_t character, float& yaw) noexcept;
// Actual local player, engine thread only. true + State{} means visible;
// false means a torn/unavailable sample or hiding without an exact door latch.
// Parent validates its Character lifetime ticket before and after this call.
bool CaptureLocalOnEngineThread(std::uintptr_t character, State& output) noexcept;
// Value-only lifecycle cleanup. Never dereferences a forgotten Character.
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;

struct DiagnosticsSnapshot final {
    std::uint64_t applied{}, failed{}, stale{}, baseline{}, interrupted{}, completed{};
    std::uint64_t transition{};
    std::uint32_t phase{}, kind{}, resourceToken{}, animationToken{}, clipPath{};
    bool active{}, playing{};
    const char* stage{"pending"};
};
[[nodiscard]] DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;
}
