#pragma once

#include "aimp/AnimationSync.h"
#include "aimp/CharacterIncarnation.h"
#include <cstdint>
#include <string>

namespace aimp { class Diagnostics; }

namespace aimp::animation_sync {

// Pass CharacterDirectStepRuntime's certified original Idle trampoline, as
// for the hiding presentation. Zero disables playback; capture still works.
void Initialize(std::uintptr_t moduleBase, std::uintptr_t nativeIdleTransition = 0U) noexcept;

// --- Owner side -------------------------------------------------------------
// Engine thread, before the character manager. Samples the actual local
// THE_PLAYER cycle: any CATHODE scripted animation (constructor 006568B0,
// vtable EADB58, state 0x12) is captured with its native parameters. Hiding
// aliases are left to the hiding presentation, which owns them.
void PumpLocalOnEngineThread() noexcept;
struct LocalSnapshot final {
    character_incarnations::Ticket ticket{};
    State state{};
    std::uint64_t capturedAt{};
    bool valid{};
};
[[nodiscard]] LocalSnapshot ReadLocal() noexcept;

// --- Observer side ----------------------------------------------------------
// Value-only mailbox (IPC thread). An inactive State clears the stay.
bool Publish(std::uintptr_t character, const State&, std::string& error) noexcept;
// Engine thread, before movement/turn/pose correction of the representations.
void PumpAllOnEngineThread() noexcept;
// True while a replicated clip owns the representation (movement must hold).
[[nodiscard]] bool OwnsMovement(std::uintptr_t character) noexcept;
// Our clip is the current cycle: carry the actor along the owner's path.
[[nodiscard]] bool ClipPlaying(std::uintptr_t character) noexcept;
// Value-only lifecycle cleanup; pinned clip requests drain on the next pump.
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;

void Export(Diagnostics& diagnostics, std::uintptr_t remoteCharacter);
}
