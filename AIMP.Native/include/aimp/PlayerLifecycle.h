#pragma once

#include <cstdint>
#include <string>

namespace aimp {

struct BuildProfile;
class Diagnostics;
class HookManager;
class SignatureResolver;

namespace player_lifecycle {

// MVP1 only: preserve native death/cleanup and bypass the automatic solo
// GameOver tail for THE_PLAYER in an existing AIMP gameplay session.
bool Install(const BuildProfile& profile, const SignatureResolver& resolver,
    HookManager& hooks, bool exactExecutableHash, std::string& error);
[[nodiscard]] bool Installed() noexcept;
// Monotonic count of suppressed local THE_PLAYER deaths (0 = none yet). The
// IPC worker publishes each new value to the Launcher as a round event.
[[nodiscard]] std::uint64_t LocalDeathSequence() noexcept;
// Retail level-exit primitives shared by every map: Survival/DLC results call
// GoToFrontend or SwitchLevel (next playlist level); campaign missions end in
// SwitchLevel. In an AIMP gameplay session the local peer must not leave alone.
inline constexpr std::uint32_t kGoToFrontendPrimitive = 0xD07EAA59U;
inline constexpr std::uint32_t kSwitchLevelPrimitive = 0xAC1AA738U;
inline constexpr std::uint32_t kTriggerInput = 0xD9D0CA29U;
[[nodiscard]] constexpr bool LevelExitPrimitive(const std::uint32_t primitive) noexcept {
    return primitive == kGoToFrontendPrimitive || primitive == kSwitchLevelPrimitive;
}
// Called on the engine thread for a level-exit primitive's trigger. Returns
// true when the retail transition must be suppressed: the local player then
// counts as having completed the mission (one more LocalLevelExitSequence).
[[nodiscard]] bool DecideLevelExit(std::uint32_t primitive) noexcept;
// Observe the executed native terminal HUD trigger, before Flash dispatch.
// This does not suppress CATHODE or require a working results movie.
void ObserveMissionComplete() noexcept;
// Monotonic count of suppressed local level exits (0 = none yet).
[[nodiscard]] std::uint64_t LocalLevelExitSequence() noexcept;
// Stop suppressing immediately. HookManager owns restoration/retirement;
// all detour data remain process-local and alive while its trampoline exists.
void Shutdown() noexcept;
// Drain rare death events on the existing bootstrap worker, never in retail.
void DrainDiagnostics(Diagnostics& diagnostics);

} // namespace player_lifecycle
} // namespace aimp
