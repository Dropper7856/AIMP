#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace aimp { class HookManager; }
namespace aimp::animation_sync::props {
// Diagnostic identity of this implementation; deliberately separate from the
// native protocol version, which the launcher compares for exact equality.
const char* BuildTag() noexcept;
// Install only with the exact validated retail module and the process-lifetime
// HookManager. Capture merely observes native bone-transform evaluations.
bool Install(HookManager&, std::uintptr_t moduleBase, std::string& error);
std::array<std::uint32_t, 4> CaptureOnEngineThread(std::uintptr_t localCharacter) noexcept;
void ReconcileOnEngineThread(std::uintptr_t character, const std::array<std::uint32_t, 4>& tokens) noexcept;
void ForgetOnEngineThread(std::uintptr_t character) noexcept;
void ResetOnEngineThread() noexcept;
void BeginLogicalShutdown() noexcept;
std::uint32_t ActiveDetours() noexcept;
}
