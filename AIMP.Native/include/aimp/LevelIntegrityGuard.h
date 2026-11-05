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
}
