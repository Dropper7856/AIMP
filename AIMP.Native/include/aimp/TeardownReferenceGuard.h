#pragma once
#include <cstdint>
#include <string>

namespace aimp { class HookManager; class Diagnostics; }
namespace aimp::teardown_reference_guard {
bool Install(std::uintptr_t base, HookManager& hooks, std::string& error);
bool Installed() noexcept;
void LogDiagnostics(Diagnostics& diagnostics);
}
