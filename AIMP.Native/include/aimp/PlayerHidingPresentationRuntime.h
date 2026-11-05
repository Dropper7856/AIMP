#pragma once

#include "aimp/PlayerHidingPresentation.h"
#include "aimp/AISyncNativeIntent.h"
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
// Exit heading: the entry heading turned half a revolution, wrapped to
// [-pi, pi] (the replicated yaw contract is |yaw| <= 3.3).
[[nodiscard]] constexpr float ExitYaw(float entryYaw) noexcept {
    constexpr float pi = 3.14159265358979F;
    return entryYaw > 0.0F ? entryYaw - pi : entryYaw + pi;
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
[[nodiscard]] constexpr std::uint32_t ClipToken(std::uint32_t kind, std::uint32_t phase) noexcept {
    if (kind == 1U) return phase == 1U ? 0xCF2A9F36U : phase == 2U ? 0x1C440762U : phase == 3U ? 0x5E642560U : 0U;
    if (kind == 2U) return phase == 1U ? 0x80645982U : phase == 2U ? 0x15691ACEU : phase == 3U ? 0x3FDD3AF4U : 0U;
    return 0U;
}
// Resource identity must come from this receiver's actual Character animation
// database. References, external clocks, signal and all camera flags stay zero.
bool BuildPlaybackIntent(const State&, std::uint32_t localResourceToken,
    ai_sync::native_intent::Payload&) noexcept;

// Pass CharacterDirectStepRuntime's already-certified original Idle trampoline
// (gEnterIdleCycle), not its hooked retail entry. It receives ECX=cycle, RET0.
// Zero disables playback admission; capture/value mailboxes remain available.
void Initialize(std::uintptr_t moduleBase, std::uintptr_t nativeIdleTransition = 0U) noexcept;
// Value-only IPC mailbox. Phase zero requests cancellation on the engine thread.
bool Publish(std::uintptr_t character, const State&, std::string& error) noexcept;
// Call before movement/turn/pose correction, even without a movement mailbox.
// True means that presentation owns movement (also true for a settled Hidden).
bool PumpOnEngineThread(std::uintptr_t character) noexcept;
void PumpAllOnEngineThread() noexcept;
// After ForgetAll, release pinned clip requests without inspecting Characters.
// If the engine cannot run again, the bounded pool remains inert until exit.
void DrainStreamsOnEngineThread() noexcept;
[[nodiscard]] bool Active(std::uintptr_t character) noexcept;
// Our entry/exit clip currently owns the actor (not a settled Hidden phase).
[[nodiscard]] bool ClipPlaying(std::uintptr_t character) noexcept;
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
