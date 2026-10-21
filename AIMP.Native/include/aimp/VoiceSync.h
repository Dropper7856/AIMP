#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aimp {
class Diagnostics;
class HookManager;
class LevelGenerationTracker;
namespace voice_sync {
bool Install(std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    Diagnostics& diagnostics, std::string& error);
bool Installed() noexcept;
bool Configure(std::span<const std::byte> bytes, bool authorityHost) noexcept;
bool QueueFrame(std::span<const std::byte> bytes) noexcept;
void TransportLost() noexcept;
void PumpOnEngineThread() noexcept;
}
}
