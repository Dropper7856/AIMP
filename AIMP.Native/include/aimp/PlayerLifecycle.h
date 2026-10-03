#pragma once

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
// Stop suppressing immediately. HookManager owns restoration/retirement;
// all detour data remain process-local and alive while its trampoline exists.
void Shutdown() noexcept;
// Drain rare death events on the existing bootstrap worker, never in retail.
void DrainDiagnostics(Diagnostics& diagnostics);

} // namespace player_lifecycle
} // namespace aimp
