#pragma once

#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::level_integrity_guard {
// Retail AI.exe fingerprints MODELS_LEVEL.BIN (level load) and DLC files and
// requests a clean quit on mismatch. Prepared levels carry the imported
// E_RIPLEY models, so both checks are bypassed in memory, exactly where the
// OpenCAGE-patched executable already bypasses them.
bool Install(std::uintptr_t base, HookManager& hooks, std::string& error);
bool Installed() noexcept;
// True when the executable already carried the OpenCAGE bypass.
bool AlreadyBypassed() noexcept;
// OpenCAGE's current-gen script fix (008D7BD5), applied in memory on retail.
bool CurrentGenScriptsEnabled() noexcept;
}

#include <vector>
namespace aimp::level_integrity_guard {
// Registers the installed developer test levels in the engine's level
// registry (as MAIN.PKG would), so load_level accepts them on the retail exe.
// Engine thread only. result: 1 registered, 0 already known, <0 refused.
struct TestLevelRegistration { std::string name; int result{}; };
std::vector<TestLevelRegistration> RegisterTestLevels(std::uintptr_t base) noexcept;
}
