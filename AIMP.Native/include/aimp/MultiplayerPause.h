#pragma once

#include <cstdint>
#include <string>

namespace aimp {
class Diagnostics;
class HookManager;
class LevelGenerationTracker;

namespace multiplayer_pause {

// Keep the retail menus, input focus and pause bookkeeping. Only certified
// simulation queries ignore the pause-menu/platform-overlay reasons, and only
// in a bound multiplayer session with a stable, active gameplay world.
bool Install(std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, std::string& error);
[[nodiscard]] bool Installed() noexcept;
// Nonzero while a retail pause/overlay menu should own the player's input:
// 1 = 0x2000/0x10000 menu reason, 2 = full 0x401 Pause_Game pair, 3 = partial
// 0x401 bits; 2/3 only with an active pause_menu/HUB owner. Pure reads.
[[nodiscard]] std::uint32_t MenuOpen(std::uintptr_t moduleBase) noexcept;
// Logical shutdown restores retail query results immediately. HookManager owns
// physical restoration; shim destinations and their immutable data stay alive.
void Shutdown() noexcept;
// Process-scoped loading token survives native level transitions. A stale release
// cannot unblock a later lobby start. Never times out into playable gameplay.
bool SetLoadingHold(bool active, const std::string& sessionId, std::uint32_t generation);
void ClearLoadingHold() noexcept;
[[nodiscard]] bool LoadingHeld() noexcept;
// Called only by MAIN_THREAD_CHARACTER_PROCESSING, using the already certified
// readiness-producer trampoline. Finishes native creation without actor ticks.
void PumpLoadingCharacterReadiness(void* readinessProducer) noexcept;
void LogDiagnostics(Diagnostics& diagnostics);

} // namespace multiplayer_pause
} // namespace aimp
