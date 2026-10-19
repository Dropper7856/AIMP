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
// Logical shutdown restores retail query results immediately. HookManager owns
// physical restoration; shim destinations and their immutable data stay alive.
void Shutdown() noexcept;
void LogDiagnostics(Diagnostics& diagnostics);

} // namespace multiplayer_pause
} // namespace aimp
